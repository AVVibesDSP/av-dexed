// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedConversion.h"
#include "AVDexedSysex.h"
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

/** A sound a folder holds: written in this conversion (`planned`, its
    index), or there before (its file and key). */
struct Kept {
    int planned = -1;
    File file;
    String key;
};

/** What a folder of the library holds already: the names taken and the sounds. */
struct FolderState {
    std::set<String> taken;              // file stems, lower case
    std::map<uint64, Kept> sounds;       // by sound key
    int nextOrder = 0;
};

FolderState readFolder(const File& folder) {
    FolderState state;
    if (! folder.isDirectory()) return state;
    for (const auto& entry : RangedDirectoryIterator(folder, false, "*", File::findFiles)) {
        const auto file = entry.getFile();
        state.taken.insert(file.getFileNameWithoutExtension().toLowerCase());
        MemoryBlock data;
        if (! AVDexedLibrary::isPresetFile(file) || file.getSize() > 32 * 1024 * 1024 || ! file.loadFileAsData(data))
            continue;
        // Sounds converted before: the same sound is not added again, and new
        // ones come after them.
        if (const auto xml = readSoundXml(data))
            if (const auto* session = xml->getChildByName("session"))
                if (const auto key = uint64(session->getStringAttribute("avSoundKey").getHexValue64()); key != 0) {
                    state.sounds[key] = { -1, file, "user:" + xml->getStringAttribute("libraryPresetId").trim() };
                    state.nextOrder = jmax(state.nextOrder, session->getIntAttribute("avLibraryOrder", -1) + 1);
                }
    }
    return state;
}

/** Where the library's converted bank voices went: one line per voice, its
    bank key, the sound file below the root and that file's key. */
File convertedVoicesFile(const File& root) {
    return root.getChildFile(AVDexedLibrary::cacheFolderName).getChildFile("converted-voices");
}

struct ConvertedVoice {
    String path, key;
};

std::map<String, ConvertedVoice> readConvertedVoices(const File& root) {
    std::map<String, ConvertedVoice> voices;
    StringArray lines;
    lines.addLines(convertedVoicesFile(root).loadFileAsString());
    for (const auto& line : lines) {
        const auto fields = StringArray::fromTokens(line, "\t", {});
        if (fields.size() == 3 && fields[0].startsWith("bank:")) voices[fields[0]] = { fields[1], fields[2] };
    }
    return voices;
}

bool writeConvertedVoices(const File& root, const std::map<String, ConvertedVoice>& voices) {
    MemoryOutputStream text;
    for (const auto& [bankKey, voice] : voices)
        if (! voice.path.containsAnyOf("\t\r\n")) text << bankKey << "\t" << voice.path << "\t" << voice.key << "\n";
    const auto file = convertedVoicesFile(root);
    return file.getParentDirectory().createDirectory() && AVDexedLibrary::writeAtomically(file, text.getMemoryBlock());
}

String relativeTo(const File& root, const File& file) {
    return file.getRelativePathFrom(root).replaceCharacter('\\', '/');
}

constexpr const char* conversionRecordName = ".conversion";

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

Result convert(const Request& request, const SoundMaker& make, const LayersMaker& makeLayers,
               const std::function<bool(float)>& progress) {
    Result result;
    struct Source {
        File file;
        std::vector<AVDexedLibrary::SourceBank> banks;
        std::vector<AVDexedPerformance::Performance> performances;
        std::shared_ptr<AVDexedPerformance::VoicePool> pool;
    };
    std::vector<Source> sources;
    int voices = 0;
    for (const auto& file : request.sources) {
        Source source { file, AVDexedLibrary::readSourceBanks(file), {}, {} };
        MemoryBlock bytes;
        if (makeLayers && file.getSize() <= AVDexedLibrary::maxImportBytes && file.loadFileAsData(bytes)) {
            const auto contents = AVDexedSysex::scan(bytes);
            source.performances = AVDexedPerformance::readPerformances(contents);
            if (! source.performances.empty())
                source.pool = std::make_shared<AVDexedPerformance::VoicePool>(file, contents);
        }
        if (source.banks.empty() && source.performances.empty()) {
            result.unreadable.add(file.getFileName());
            continue;
        }
        for (const auto& bank : source.banks) voices += bank.voices;
        voices += int(source.performances.size());
        result.banks += int(source.banks.size());
        result.performances += int(source.performances.size());
        sources.push_back(std::move(source));
    }

    int realBanks = 0;
    for (const auto& source : sources) {
        for (const auto& bank : source.banks) realBanks += bank.singleVoices ? 0 : 1;
        realBanks += source.performances.empty() ? 0 : 1;
    }
    const auto layout = request.layout != Layout::automatic ? request.layout
                      : realBanks <= 1 ? Layout::flat : Layout::folderPerBank;

    // Everything goes into a hidden folder of the library first (the same
    // disk: moving in is a rename), and only then into the library.
    const auto staging = request.root.getChildFile(".converting-" + Uuid().toString());
    struct Planned {
        File staged, target;
        String key, name;
        int order = 0;
    };
    std::vector<Planned> planned;
    struct PendingOrigin {
        String bankKey;
        Kept kept;
    };
    std::vector<PendingOrigin> pendingOrigins;
    std::map<String, FolderState> folders;
    const auto fail = [&]() {
        staging.deleteRecursively();
        result.ok = false;
        result.sounds = 0;
        result.files.clear();
        result.folders.clear();
        result.origins.clear();
        return result;
    };
    const auto folderState = [&folders](const File& folder) -> FolderState& {
        auto found = folders.find(folder.getFullPathName());
        if (found == folders.end())
            found = folders.emplace(folder.getFullPathName(), readFolder(folder)).first;
        return found->second;
    };
    // Staged under its future name; the same sound already in the folder is
    // not written again. Returns false when the staging cannot be written.
    const auto stage = [&](FolderState& state, const File& folder, const String& name, const MadeSound& made,
                           const String& bankKey) {
        if (const auto there = state.sounds.find(made.key); there != state.sounds.end()) {
            state.taken.erase(name.toLowerCase());
            ++result.repeated;
            if (bankKey.isNotEmpty()) pendingOrigins.push_back({ bankKey, there->second });
            return true;
        }
        const auto staged = staging.getChildFile(String(int(planned.size()))).withFileExtension(AVDexedLibrary::presetExtension);
        if (! staging.createDirectory() || ! staged.replaceWithData(made.state.getData(), made.state.getSize()))
            return false;
        state.sounds[made.key] = { int(planned.size()), {}, {} };
        if (bankKey.isNotEmpty()) pendingOrigins.push_back({ bankKey, { int(planned.size()), {}, {} } });
        planned.push_back({ staged, folder.getChildFile(name + AVDexedLibrary::presetExtension), "user:" + made.id, name,
                            state.nextOrder++ });
        return true;
    };
    int done = 0;
    const auto keepGoing = [&]() {
        if (progress && ! progress(voices > 0 ? float(done) / float(voices) : 1.0f)) {
            result.cancelled = true;
            return false;
        }
        ++done;
        return true;
    };
    for (const auto& source : sources) {
        const auto base = layout == Layout::besideSources ? source.file.getParentDirectory()
                                                          : mirrored(request.destination, source.file, request.sourceBase);
        const bool ownFolders = layout == Layout::folderPerBank;
        for (const auto& bank : source.banks) {
            const auto folderName = AVDexedLibrary::sanitizeFolderName(bank.name);
            const auto folder = ! ownFolders || bank.singleVoices ? base
                              : base.getChildFile(folderName.isNotEmpty() ? folderName : String("Bank"));
            auto& state = folderState(folder);
            const auto names = voiceNames(bank.bank, bank.voices, bank.name);
            const auto fingerprint = AVDexedLibrary::bankFingerprint(bank.bank);
            for (int slot = 0; slot < bank.voices; ++slot) {
                if (! keepGoing()) return fail();
                if (isInitVoice(bank.bank, slot)) {
                    ++result.empty;
                    continue;
                }
                auto stem = fileStem(names[slot]);
                if (stem.isEmpty()) stem = fileStem(bank.name + " " + String(slot + 1));
                if (stem.isEmpty()) stem = "Sound " + String(slot + 1);
                const auto name = freeStem(state, stem);
                const auto made = make(bank, slot, name, state.nextOrder);
                if (made.state.isEmpty() || ! stage(state, folder, name, made, "bank:" + fingerprint + ":" + String(slot).paddedLeft('0', 2)))
                    return fail();
            }
        }
        if (source.performances.empty()) continue;
        const auto stem = source.file.getFileNameWithoutExtension();
        const auto folderName = AVDexedLibrary::sanitizeFolderName(source.banks.empty() ? stem : stem + " Performances");
        const auto folder = ! ownFolders ? base : base.getChildFile(folderName.isNotEmpty() ? folderName : String("Performances"));
        auto& state = folderState(folder);
        for (const auto& performance : source.performances) {
            if (! keepGoing()) return fail();
            auto stemName = fileStem(performance.name.trim());
            if (stemName.isEmpty()) stemName = fileStem(stem + " " + String(performance.number));
            if (stemName.isEmpty()) stemName = "Performance " + String(performance.number);
            const auto name = freeStem(state, stemName);
            const auto made = makeLayers(AVDexedPerformance::resolve(performance, *source.pool), name, stem,
                                         performance.number, state.nextOrder);
            if (made.state.isEmpty() || ! stage(state, folder, name, made, {}))
                return fail();
        }
    }

    // The backup: copies of the originals, as they were, before anything
    // changes in the library; or, for the library's own banks, the originals
    // themselves, moved there once their sounds are in place.
    const auto backupPathOf = [&request](const File& source) {
        const bool below = request.sourceBase != File() && source.isAChildOf(request.sourceBase);
        return below ? source.getRelativePathFrom(request.sourceBase).replaceCharacter('\\', '/') : source.getFileName();
    };
    // What this conversion copied into the backup. A failure removes only these,
    // and the folder only once empty: it may hold an original that could not go
    // back to its place.
    std::vector<File> copied;
    const auto dropBackup = [&]() {
        for (const auto& file : copied) file.deleteFile();
        if (result.backup != File() && result.backup.findChildFiles(File::findFiles, true).isEmpty())
            result.backup.deleteRecursively();
        result.backup = File();
    };
    if (request.backup && ! sources.empty()) {
        const auto backups = request.root.getChildFile(AVDexedLibrary::syxBackupsFolderName);
        auto label = Time::getCurrentTime().formatted("%Y-%m-%d %H%M");
        if (const auto clean = AVDexedLibrary::sanitizeFolderName(request.backupLabel); clean.isNotEmpty())
            label << " " << clean;
        {
            // Chosen and made under the library's lock: another AV Dexed
            // converting in the same minute gets a folder of its own.
            const AVDexedLibrary::LibraryScope lock(10000);
            if (lock.held) {
                result.backup = AVDexedLibrary::uniqueDestination(backups, label);
                if (! result.backup.createDirectory()) result.backup = File();
            }
        }
        if (result.backup == File()) return fail();
        if (! request.moveSources)
            for (const auto& source : sources) {
                auto target = result.backup.getChildFile(backupPathOf(source.file));
                target = AVDexedLibrary::uniqueDestination(target.getParentDirectory(), target.getFileName());
                if (! target.getParentDirectory().createDirectory() || ! source.file.copyFileTo(target)) {
                    dropBackup();
                    return fail();
                }
                copied.push_back(target);
            }
    }

    {
        const AVDexedLibrary::LibraryScope lock(10000);
        std::vector<File> moved, created;
        std::vector<std::pair<File, File>> movedSources;   // from, to
        const auto undo = [&]() {
            for (const auto& file : moved) file.deleteFile();
            for (const auto& [from, to] : movedSources) to.moveFileTo(from);
            // The folders made for them, deepest first, while they are empty.
            std::sort(created.begin(), created.end(), [](const File& a, const File& b) {
                return a.getFullPathName().length() > b.getFullPathName().length();
            });
            for (const auto& folder : created)
                if (folder.isDirectory() && folder.getNumberOfChildFiles(File::findFilesAndDirectories) == 0) folder.deleteFile();
            dropBackup();
            return fail();
        };
        const auto makeFolderFor = [&created](const File& file) {
            for (auto folder = file.getParentDirectory(); ! folder.exists() && folder != folder.getParentDirectory();
                 folder = folder.getParentDirectory())
                created.push_back(folder);
            return file.getParentDirectory().createDirectory().wasOk();
        };
        if (! lock.held) return undo();
        for (auto& item : planned) {
            // Taken meanwhile (another AV Dexed, Finder): never overwritten.
            if (item.target.exists()) item.target = AVDexedLibrary::uniqueDestination(item.target.getParentDirectory(), item.target.getFileName());
            if (! makeFolderFor(item.target) || ! item.staged.moveFileTo(item.target))
                return undo();
            moved.push_back(item.target);
            result.files.add(item.target);
            if (! result.folders.contains(item.target.getParentDirectory())) result.folders.add(item.target.getParentDirectory());
        }
        if (request.moveSources && result.backup != File()) {
            for (const auto& source : sources) {
                auto target = result.backup.getChildFile(backupPathOf(source.file));
                target = AVDexedLibrary::uniqueDestination(target.getParentDirectory(), target.getFileName());
                if (! target.getParentDirectory().createDirectory() || ! source.file.moveFileTo(target))
                    return undo();
                movedSources.emplace_back(source.file, target);
            }
            // What undoConversion needs: where each original was, and each
            // sound file made (its size and time: one saved over since stays).
            MemoryOutputStream record;
            for (const auto& [from, to] : movedSources)
                record << "source\t" << relativeTo(request.root, from) << "\t" << relativeTo(result.backup, to) << "\n";
            for (const auto& file : moved)
                record << "sound\t" << relativeTo(request.root, file) << "\t" << String(file.getSize()) << "\t"
                       << String(file.getLastModificationTime().toMilliseconds()) << "\n";
            if (! AVDexedLibrary::writeAtomically(result.backup.getChildFile(conversionRecordName), record.getMemoryBlock()))
                return undo();
        }
    }
    staging.deleteRecursively();
    result.sounds = result.files.size();
    for (const auto& origin : pendingOrigins) {
        if (origin.kept.planned >= 0) {
            const auto& item = planned[size_t(origin.kept.planned)];
            result.origins.push_back({ origin.bankKey, item.target, item.key });
        } else {
            result.origins.push_back({ origin.bankKey, origin.kept.file, origin.kept.key });
        }
    }
    // A DAW session that played one of the library's bank voices finds the
    // sound it became; the browser's index knows the new files already.
    if (request.moveSources) {
        auto known = readConvertedVoices(request.root);
        for (const auto& origin : result.origins)
            known[origin.bankKey] = { relativeTo(request.root, origin.file), origin.key };
        writeConvertedVoices(request.root, known);
    }
    if (! planned.empty()) {
        auto index = AVDexedLibrary::readSoundIndex(request.root);
        for (const auto& item : planned)
            if (item.target.isAChildOf(request.root)) {
                const auto path = relativeTo(request.root, item.target);
                index[path] = { path, item.target.getSize(), item.target.getLastModificationTime().toMilliseconds(),
                                item.order, item.key, item.name };
            }
        std::vector<AVDexedLibrary::IndexedSound> sounds;
        for (const auto& [path, sound] : index) sounds.push_back(sound);
        AVDexedLibrary::writeSoundIndex(request.root, sounds);
    }
    result.ok = true;
    if (progress) progress(1.0f);
    return result;
}

File convertedVoice(const File& root, const String& bankKey, String* key) {
    // Read once per change of the record (a session restore, the start of an instance).
    static CriticalSection lock;
    static std::map<String, ConvertedVoice> voices;
    static String readFrom;
    const auto file = convertedVoicesFile(root);
    const auto stamp = file.getFullPathName() + "|" + String(file.getSize()) + "|" + String(file.getLastModificationTime().toMilliseconds());
    const ScopedLock scope(lock);
    if (stamp != readFrom) {
        voices = readConvertedVoices(root);
        readFrom = stamp;
    }
    const auto found = voices.find(bankKey);
    if (found == voices.end()) return {};
    const auto sound = root.getChildFile(found->second.path);
    if (! sound.existsAsFile()) return {};
    if (key != nullptr) *key = found->second.key;
    return sound;
}

UndoResult undoConversion(const File& root, const File& backup) {
    UndoResult result;
    const auto recordFile = backup.getChildFile(conversionRecordName);
    StringArray lines;
    lines.addLines(recordFile.loadFileAsString());
    if (lines.isEmpty()) return result;
    const AVDexedLibrary::LibraryScope lock(10000);
    if (! lock.held) return result;
    std::set<String> removedPaths;
    for (const auto& line : lines) {
        const auto fields = StringArray::fromTokens(line, "\t", {});
        if (fields.size() == 4 && fields[0] == "sound") {
            const auto file = root.getChildFile(fields[1]);
            if (! file.existsAsFile()) continue;
            // Saved over since (an edit, "Guardar"): that one is the user's now.
            if (file.getSize() == fields[2].getLargeIntValue() && file.getLastModificationTime().toMilliseconds() == fields[3].getLargeIntValue()
                && file.deleteFile()) {
                ++result.removed;
                removedPaths.insert(fields[1]);
            } else {
                ++result.kept;
            }
        }
    }
    for (const auto& line : lines) {
        const auto fields = StringArray::fromTokens(line, "\t", {});
        if (fields.size() == 3 && fields[0] == "source") {
            const auto from = backup.getChildFile(fields[2]), to = root.getChildFile(fields[1]);
            if (! from.existsAsFile() || to.exists()) continue;
            if (to.getParentDirectory().createDirectory() && from.moveFileTo(to)) ++result.restored;
        }
    }
    // The bank voices that are banks again: their favourites go back to them.
    auto known = readConvertedVoices(root);
    for (auto it = known.begin(); it != known.end();) {
        if (removedPaths.count(it->second.path) > 0) {
            result.origins.push_back({ it->first, root.getChildFile(it->second.path), it->second.key });
            it = known.erase(it);
        } else {
            ++it;
        }
    }
    writeConvertedVoices(root, known);
    recordFile.deleteFile();
    if (backup.findChildFiles(File::findFiles, true).isEmpty()) backup.deleteRecursively();
    result.ok = true;
    return result;
}

File lastUndoableConversion(const File& root) {
    File newest;
    for (const auto& folder : root.getChildFile(AVDexedLibrary::syxBackupsFolderName).findChildFiles(File::findDirectories, false))
        if (folder.getChildFile(conversionRecordName).existsAsFile()
            && (newest == File() || folder.getFileName().compareNatural(newest.getFileName()) > 0))
            newest = folder;
    return newest;
}
}
