// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedConversion.h"
#include "Program.h"
#include <map>
#include <set>

using namespace juce;

namespace AVDexedConversion {
namespace {
/** "26 BAS Ana" → 26 and "BAS Ana"; -1 for a name without such a number. */
int leadingNumber(const String& name, String& rest) {
    const auto digits = name.initialSectionContainingOnly("0123456789");
    if (digits.isEmpty() || digits.length() > 2 || name.length() <= digits.length() + 1
        || name[digits.length()] != ' ')
        return -1;
    rest = name.substring(digits.length() + 1).trim();
    return rest.isEmpty() ? -1 : digits.getIntValue();
}

/** A voice that only holds Init: an empty slot. */
bool isInitVoice(const Cartridge& bank, int slot) {
    static const auto init = Program::initialProgram();
    return std::memcmp(Cartridge(bank).unpackProgram(slot).getRawData(), init.getRawData(), 145) == 0;
}

/** A sound file's XML (AudioProcessor::copyXmlToBinary: a magic number, the
    length, then the text), without the audio modules. */
std::unique_ptr<XmlElement> readSoundXml(const MemoryBlock& data) {
    constexpr uint32 magic = 0x21324356;
    if (data.getSize() <= 8 || ByteOrder::littleEndianInt(data.getData()) != magic) return {};
    const auto length = int(ByteOrder::littleEndianInt(addBytesToPointer(data.getData(), 4)));
    if (length <= 0) return {};
    return parseXML(String::fromUTF8(static_cast<const char*>(data.getData()) + 8, jmin(int(data.getSize()) - 8, length)));
}

/** A sound name as a file name ("" when nothing usable is left). */
String fileStem(const String& name) {
    return AVDexedLibrary::sanitizeFolderName(name);
}

/** What a folder of the library holds already: the names taken and the sounds. */
struct FolderState {
    std::set<String> taken;              // file stems, lower case
    std::set<uint64> keys;
    int nextOrder = 0;
};

FolderState readFolder(const File& folder) {
    FolderState state;
    if (! folder.isDirectory()) return state;
    for (const auto& entry : RangedDirectoryIterator(folder, false, "*", File::findFiles)) {
        const auto file = entry.getFile();
        state.taken.insert(file.getFileNameWithoutExtension().toLowerCase());
        if (AVDexedLibrary::isPresetFile(file))
            if (const auto key = readSoundKey(file); key != 0) state.keys.insert(key);
    }
    return state;
}

/** `stem`, or "stem 2", "stem 3"… when the folder already has that name. */
String freeStem(FolderState& state, const String& stem) {
    auto candidate = stem;
    for (int n = 2; state.taken.count(candidate.toLowerCase()) > 0; ++n)
        candidate = stem + " " + String(n);
    state.taken.insert(candidate.toLowerCase());
    return candidate;
}

/** `folder` below `base`, as safe folder names (a dropped folder's structure). */
File mirrored(const File& destination, const File& source, const File& base) {
    if (base == File() || ! source.isAChildOf(base)) return destination;
    auto target = destination;
    for (const auto& part : StringArray::fromTokens(source.getParentDirectory().getRelativePathFrom(base), "/\\", {}))
        if (const auto name = AVDexedLibrary::sanitizeFolderName(part); name.isNotEmpty())
            target = target.getChildFile(name);
    return target;
}
}

StringArray voiceNames(const Cartridge& bank, int count, const String& fallback) {
    count = jlimit(0, 32, count);
    StringArray raw;
    Cartridge(bank).getProgramNames(raw);
    StringArray names;
    for (int slot = 0; slot < count; ++slot)
        names.add(slot < raw.size() ? raw[slot].trimEnd() : String());
    // Numbered from 0 or from 1, by most of its voices: a name of its own that
    // happens to start with a number ("2 PIANOS") is not a numbered bank.
    int fromZero = 0, fromOne = 0;
    for (int slot = 0; slot < count; ++slot) {
        String rest;
        const int number = leadingNumber(names[slot], rest);
        fromZero += number == slot ? 1 : 0;
        fromOne += number == slot + 1 ? 1 : 0;
    }
    const int needed = jmax(2, (count * 3 + 3) / 4);
    const int first = fromZero >= needed ? 0 : fromOne >= needed ? 1 : -1;
    for (int slot = 0; slot < count; ++slot) {
        String rest;
        if (first >= 0 && leadingNumber(names[slot], rest) == slot + first)
            names.set(slot, rest);
        if (names[slot].trim().isEmpty())
            names.set(slot, fallback + " " + String(slot + 1));
    }
    return names;
}

uint64 readSoundKey(const File& soundFile) {
    MemoryBlock data;
    if (soundFile.getSize() > 32 * 1024 * 1024 || ! soundFile.loadFileAsData(data)) return 0;
    if (const auto xml = readSoundXml(data))
        if (const auto* session = xml->getChildByName("session"))
            return uint64(session->getStringAttribute("avSoundKey").getHexValue64());
    return 0;
}

Result convert(const Request& request, const SoundMaker& make, const std::function<bool(float)>& progress) {
    Result result;
    struct Source {
        File file;
        std::vector<AVDexedLibrary::SourceBank> banks;
    };
    std::vector<Source> sources;
    int voices = 0;
    for (const auto& file : request.sources) {
        auto banks = AVDexedLibrary::readSourceBanks(file);
        if (banks.empty()) {
            result.unreadable.add(file.getFileName());
            continue;
        }
        for (const auto& bank : banks) voices += bank.voices;
        result.banks += int(banks.size());
        sources.push_back({ file, std::move(banks) });
    }

    // Everything goes into a hidden folder of the library first (the same
    // disk: moving in is a rename), and only then into the library.
    const auto staging = request.root.getChildFile(".converting-" + Uuid().toString());
    struct Planned {
        File staged, target;
    };
    std::vector<Planned> planned;
    std::map<String, FolderState> folders;
    const auto fail = [&]() {
        staging.deleteRecursively();
        result.ok = false;
        result.sounds = 0;
        result.files.clear();
        result.folders.clear();
        return result;
    };
    int done = 0;
    for (const auto& source : sources) {
        const auto base = mirrored(request.destination, source.file, request.sourceBase);
        for (const auto& bank : source.banks) {
            const auto folderName = AVDexedLibrary::sanitizeFolderName(bank.name);
            const auto folder = request.layout == Layout::flat ? base
                              : base.getChildFile(folderName.isNotEmpty() ? folderName : String("Bank"));
            auto found = folders.find(folder.getFullPathName());
            if (found == folders.end())
                found = folders.emplace(folder.getFullPathName(), readFolder(folder)).first;
            auto& state = found->second;
            const auto names = voiceNames(bank.bank, bank.voices, bank.name);
            for (int slot = 0; slot < bank.voices; ++slot) {
                if (progress && ! progress(voices > 0 ? float(done) / float(voices) : 1.0f))
                    return fail();
                ++done;
                if (isInitVoice(bank.bank, slot)) {
                    ++result.empty;
                    continue;
                }
                auto stem = fileStem(names[slot]);
                if (stem.isEmpty()) stem = fileStem(bank.name + " " + String(slot + 1));
                if (stem.isEmpty()) stem = "Sound " + String(slot + 1);
                const auto name = freeStem(state, stem);
                const auto made = make(bank, slot, name, state.nextOrder);
                if (made.state.isEmpty())
                    return fail();
                if (! state.keys.insert(made.key).second) {
                    state.taken.erase(name.toLowerCase());
                    ++result.repeated;
                    continue;
                }
                ++state.nextOrder;
                const auto staged = staging.getChildFile(String(int(planned.size()))).withFileExtension(AVDexedLibrary::presetExtension);
                if (! staging.createDirectory() || ! staged.replaceWithData(made.state.getData(), made.state.getSize()))
                    return fail();
                planned.push_back({ staged, folder.getChildFile(name + AVDexedLibrary::presetExtension) });
            }
        }
    }

    // The originals, as they were, before anything changes in the library.
    if (request.backup && ! sources.empty()) {
        const auto backups = request.root.getChildFile(AVDexedLibrary::syxBackupsFolderName);
        auto label = Time::getCurrentTime().formatted("%Y-%m-%d %H%M");
        if (const auto clean = AVDexedLibrary::sanitizeFolderName(request.backupLabel); clean.isNotEmpty())
            label << " " << clean;
        result.backup = AVDexedLibrary::uniqueDestination(backups, label);
        for (const auto& source : sources) {
            const bool below = request.sourceBase != File() && source.file.isAChildOf(request.sourceBase);
            auto target = below ? result.backup.getChildFile(source.file.getRelativePathFrom(request.sourceBase))
                                : result.backup.getChildFile(source.file.getFileName());
            target = AVDexedLibrary::uniqueDestination(target.getParentDirectory(), target.getFileName());
            if (! target.getParentDirectory().createDirectory() || ! source.file.copyFileTo(target)) {
                result.backup.deleteRecursively();
                result.backup = File();
                return fail();
            }
        }
    }

    {
        const AVDexedLibrary::LibraryScope lock(10000);
        std::vector<File> moved;
        const auto undo = [&]() {
            for (const auto& file : moved) file.deleteFile();
            if (result.backup != File()) result.backup.deleteRecursively();
            result.backup = File();
            return fail();
        };
        if (! lock.held) return undo();
        for (const auto& item : planned) {
            auto target = item.target;
            // Taken meanwhile (another AV Dexed, Finder): never overwritten.
            if (target.exists()) target = AVDexedLibrary::uniqueDestination(target.getParentDirectory(), target.getFileName());
            if (! target.getParentDirectory().createDirectory() || ! item.staged.moveFileTo(target))
                return undo();
            moved.push_back(target);
            result.files.add(target);
            if (! result.folders.contains(target.getParentDirectory())) result.folders.add(target.getParentDirectory());
        }
    }
    staging.deleteRecursively();
    result.sounds = result.files.size();
    result.ok = true;
    if (progress) progress(1.0f);
    return result;
}
}
