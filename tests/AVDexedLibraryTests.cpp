// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// Sound library on disk: names, copy-on-import, real folders. Everything runs in
// a fresh temporary folder, never in the user's data.

#include "../source/core/AVDexedLibrary.h"
#include "../source/core/Program.h"

#include <iostream>

#if JUCE_WINDOWS
 #include <chrono>
 #include <thread>
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace {
bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

/** A temporary folder that is removed afterwards (only ever under /tmp). */
struct ScratchFolder {
    juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("avdexed-library-" + juce::Uuid().toString());
    ScratchFolder() { root.createDirectory(); }
    ~ScratchFolder() { if (root.getFullPathName().contains("avdexed-library-")) root.deleteRecursively(); }
};

juce::MemoryBlock makeBank(int variant)
{
    Cartridge bank;
    auto program = Program::initialProgram();
    for (int slot = 0; slot < 32; ++slot)
        bank.packProgram(program.getRawData(), slot, "VOICE " + juce::String(slot + variant));
    juce::MemoryBlock bytes(bank.getVoiceSysex(), SYSEX_SIZE);
    return bytes;
}

bool testFolderNames()
{
    using AVDexedLibrary::sanitizeFolderName;
    return expect(sanitizeFolderName("  Mis pianos  ") == "Mis pianos", "folder names are trimmed")
        && expect(sanitizeFolderName("a/b\\c:d*e?f\"g<h>i|j") == "abcdefghij", "path and reserved characters are removed")
        && expect(sanitizeFolderName("...hidden..") == "hidden", "leading and trailing dots are removed")
        && expect(sanitizeFolderName("CON").isEmpty() && sanitizeFolderName("nul.txt").isEmpty(), "Windows device names are refused")
        && expect(sanitizeFolderName("   ").isEmpty() && sanitizeFolderName("..").isEmpty(), "empty or dot names are refused")
        && expect(sanitizeFolderName(juce::String::repeatedString("x", 80)).length() == 48, "names are limited to 48 characters")
        && expect(sanitizeFolderName(juce::String::fromUTF8("Órbita ñandú")) == juce::String::fromUTF8("Órbita ñandú"), "accents are kept");
}

bool testImportCopiesWithoutOverwriting()
{
    ScratchFolder scratch;
    const auto root = AVDexedLibrary::rootFor(scratch.root);
    const auto outside = scratch.root.getChildFile("Downloads");
    outside.createDirectory();
    const auto source = outside.getChildFile("Finetales.syx");
    const auto bankA = makeBank(0), bankB = makeBank(100);
    source.replaceWithData(bankA.getData(), bankA.getSize());
    const auto folder = root.getChildFile("Finetales");

    const auto first = AVDexedLibrary::importBank(source, folder);
    juce::MemoryBlock copied;
    first.file.loadFileAsData(copied);
    bool passed = expect(first.status == AVDexedLibrary::ImportResult::Status::copied
                             && first.file == folder.getChildFile("Finetales.syx") && copied == bankA,
                         "a bank from outside is copied byte for byte into the folder");
    passed &= expect(source.existsAsFile(), "the original file stays where it was");

    const auto again = AVDexedLibrary::importBank(source, folder);
    passed &= expect(again.status == AVDexedLibrary::ImportResult::Status::alreadyThere && again.file == first.file,
                     "importing the same bank twice reuses the copy");

    source.replaceWithData(bankB.getData(), bankB.getSize());
    const auto different = AVDexedLibrary::importBank(source, folder);
    juce::MemoryBlock kept;
    first.file.loadFileAsData(kept);
    passed &= expect(different.status == AVDexedLibrary::ImportResult::Status::copied
                         && different.file == folder.getChildFile("Finetales 2.syx") && kept == bankA,
                     "a different bank with the same name gets ' 2' and never overwrites");

    const auto inside = AVDexedLibrary::importBank(first.file, folder);
    passed &= expect(inside.status == AVDexedLibrary::ImportResult::Status::alreadyThere && inside.file == first.file,
                     "a bank already in the target folder is not copied again");

    const auto junk = outside.getChildFile("notes.syx");
    junk.replaceWithText("not a bank");
    passed &= expect(AVDexedLibrary::importBank(junk, folder).status == AVDexedLibrary::ImportResult::Status::notABank,
                     "files that are not DX7 banks are refused");
    passed &= expect(AVDexedLibrary::listFolders(root).size() == 2, "refused or reused imports create no extra folders");
    return passed;
}

bool testRealFolders()
{
    ScratchFolder scratch;
    const auto root = AVDexedLibrary::rootFor(scratch.root);
    for (auto name : { "SynprezFM", "dexed", "Mis pianos", ".hidden", "__MACOSX", "Pack.installing" })
        root.getChildFile(name).createDirectory();
    root.getChildFile("loose.syx").replaceWithText("x");
    juce::StringArray names;
    for (const auto& folder : AVDexedLibrary::listFolders(root)) names.add(folder.getFileName());
    bool passed = expect(names.joinIntoString("|") == "dexed|Mis pianos|SynprezFM",
                         "the library lists the real first-level folders, sorted, without hidden or temporary ones");
    passed &= expect(AVDexedLibrary::folderOf(root.getChildFile("SynprezFM/sub/a.syx"), root) == "SynprezFM/sub",
                     "files in subfolders keep their relative folder");
    passed &= expect(AVDexedLibrary::folderOf(root.getChildFile("loose.syx"), root).isEmpty(),
                     "files loose in the root belong to no folder");
    passed &= expect(AVDexedLibrary::uniqueDestination(root, "loose.syx") == root.getChildFile("loose 2.syx"),
                     "unique destinations append a number");
    return passed;
}

juce::File writeBankAt(const juce::File& root, const char* relativePath, int variant = 0)
{
    const auto file = root.getChildFile(relativePath);
    file.getParentDirectory().createDirectory();
    const auto bytes = makeBank(variant);
    file.replaceWithData(bytes.getData(), bytes.getSize());
    return file;
}

juce::String relativeList(const juce::Array<juce::File>& files, const juce::File& root)
{
    juce::StringArray relative;
    for (const auto& file : files) relative.add(file.getRelativePathFrom(root).replaceCharacter('\\', '/'));
    return relative.joinIntoString("|");
}

bool testBrowserScan()
{
    ScratchFolder scratch;
    const auto root = AVDexedLibrary::rootFor(scratch.root);
    writeBankAt(root, "Pianos/Rhodes/Suitcase.syx");
    writeBankAt(root, "Pianos/Profiles/Kept.syx");
    writeBankAt(root, "Pianos/Grand.SYX");
    root.getChildFile("Pianos/Mine.dexedpreset").replaceWithText("preset");
    root.getChildFile("Pianos/notes.txt").replaceWithText("notes");
    writeBankAt(root, "loose.syx");
    for (auto hidden : { ".deleted/Old.syx", ".profiles/P.syx", "__MACOSX/M.syx", "Pack.installing/I.syx", "Profiles/X.syx" })
        writeBankAt(root, hidden);
    const auto expected = juce::String("loose.syx|Pianos/Grand.SYX|Pianos/Mine.dexedpreset|Pianos/Profiles/Kept.syx|Pianos/Rhodes/Suitcase.syx");
    bool passed = expect(relativeList(AVDexedLibrary::findSoundFiles(root), root) == expected,
                         "the browser finds .syx (any case) and .dexedpreset at any depth, skipping hidden, temporary and reserved names");
    passed &= expect(relativeList(AVDexedLibrary::listFolders(root, true), root) == "Pianos|Pianos/Profiles|Pianos/Rhodes",
                     "nested folders are listed after their parent; only the root's own Profiles is reserved");
    if (root.createSymbolicLink(root.getChildFile("Pianos/Loop"), false)) {
        passed &= expect(relativeList(AVDexedLibrary::findSoundFiles(root), root) == expected
                             && AVDexedLibrary::listFolders(root, true).size() == 3,
                         "a symbolic link back to the root does not make the scan endless or duplicate sounds");
        root.getChildFile("Pianos/Loop").deleteFile();
    }
    return passed;
}

/** One order of names on every system (the browser's rows, and the sound a
    program change plays): ASCII names as natural order always had them,
    accented letters beside their plain ones however the accent is stored. */
bool testNameOrder()
{
    const auto sorted = [](juce::StringArray names) {
        std::sort(names.begin(), names.end(), [](const juce::String& a, const juce::String& b) {
            return AVDexedLibrary::compareSoundNames(a, b) < 0;
        });
        return names.joinIntoString("|");
    };
    // None of these differ only in case or spaces, where natural order has no say.
    const juce::StringArray ascii { "Bass 10", "bass 2", "Bass 1", "_Intro", "a-b", "A b", "Zeta", "alpha", "Pad 007",
                                    "Pad 07", "Pad 7", "Pad", "pad 2b", "Pad 2a", "~x", "10cc", "9 to 5", "(Live)" };
    auto natural = ascii;
    std::sort(natural.begin(), natural.end(), [](const juce::String& a, const juce::String& b) {
        return a.compareNatural(b, false) < 0;
    });
    bool passed = expect(sorted(ascii) == natural.joinIntoString("|"), "ASCII names keep the natural order they always had");

    const auto text = [](const char* utf8) { return juce::String::fromUTF8(utf8); };
    const auto list = [&text](std::initializer_list<const char*> utf8) {
        juce::StringArray names;
        for (const auto* name : utf8) names.add(text(name));
        return names;
    };
    passed &= expect(sorted(list({ "Órgano", "Bajo", "Ácido", "Zampoña", "Acordeón" })) == text("Ácido|Acordeón|Bajo|Órgano|Zampoña"),
                     "an accented letter sorts beside its plain letter, as Finder and Explorer show it");
    // As macOS may store names: the letter, then its accent (U+0301).
    const auto apart = list({ "Acordeón", "O\xcc\x81" "rgano", "Bajo", "A\xcc\x81" "cido" });
    passed &= expect(sorted(apart) == apart[3] + "|" + apart[0] + "|" + apart[2] + "|" + apart[1],
                     "an accent stored apart from its letter sorts the same");
    passed &= expect(sorted(list({ "Peña", "ola", "Pena", "Ola" })) == text("Ola|ola|Pena|Peña"),
                     "names equal but for accents or case still get one order");
    passed &= expect(sorted(list({ "Alpha", "🎹 Keys", "·Intro" })) == text("·Intro|🎹 Keys|Alpha"),
                     "signs and emoji go before letters, as other punctuation does");

    ScratchFolder scratch;
    const auto root = AVDexedLibrary::rootFor(scratch.root);
    const auto bank = makeBank(0);
    for (const auto& name : list({ "Pads/Órgano.syx", "Pads/Bajo.syx", "Pads/Ácido.syx", "Ébano/Uno.syx", "Bosque/Dos.syx" })) {
        const auto file = root.getChildFile(name);
        file.getParentDirectory().createDirectory();
        file.replaceWithData(bank.getData(), bank.getSize());
    }
    passed &= expect(relativeList(AVDexedLibrary::findSoundFiles(root), root)
                         == text("Bosque/Dos.syx|Ébano/Uno.syx|Pads/Ácido.syx|Pads/Bajo.syx|Pads/Órgano.syx"),
                     "the browser lists accented folders and sounds in that order");
    return passed;
}

bool testLibrarySignature()
{
    ScratchFolder scratch;
    const auto root = AVDexedLibrary::rootFor(scratch.root);
    writeBankAt(root, "Pianos/Rhodes/Suitcase.syx");
    writeBankAt(root, "loose.syx");
    const auto before = AVDexedLibrary::librarySignature(root);
    juce::Thread::sleep(20);   // folder times have millisecond resolution here
    root.getChildFile("AV Dexed Preferences.xml").replaceWithText("<PROPERTIES/>");
    root.getChildFile(".deleted").createDirectory();
    bool passed = expect(AVDexedLibrary::librarySignature(root) == before,
                         "rewriting the preferences or touching hidden folders is not a library change");
    writeBankAt(root, "Pianos/Rhodes/Second.syx", 7);
    const auto nestedChange = AVDexedLibrary::librarySignature(root);
    passed &= expect(nestedChange != before, "a sound added two levels down changes the signature");
    juce::Thread::sleep(20);
    writeBankAt(root, "loose.syx", 9);
    passed &= expect(AVDexedLibrary::librarySignature(root) != nestedChange, "a loose sound rewritten in the root changes it");
    const auto beforeRename = AVDexedLibrary::librarySignature(root);
    root.getChildFile("Pianos/Rhodes").moveFileTo(root.getChildFile("Pianos/Wurli"));
    passed &= expect(AVDexedLibrary::librarySignature(root) != beforeRename, "renaming a nested folder changes it");
    return passed;
}

bool testFolderOperations()
{
    ScratchFolder scratch;
    const auto root = AVDexedLibrary::rootFor(scratch.root);
    using AVDexedLibrary::RenameResult;
    const auto suitcase = writeBankAt(root, "Pianos/Rhodes/Suitcase.syx");
    writeBankAt(root, "Pianos/Grand.syx", 3);
    root.getChildFile("Pianos/Pads").createDirectory();
    root.getChildFile("Pianos/Mine.dexedpreset").replaceWithText("mine");
    root.getChildFile("Pianos/Other.dexedpreset").replaceWithText("other");
    root.getChildFile("Pianos/readme.txt").replaceWithText("text");
    root.getChildFile("Pianos/.DS_Store").replaceWithText("");
    writeBankAt(root, "Pianos/.old/Hidden.syx");
    writeBankAt(root, "SynprezFM/SynprezFM_01.syx", 5);
    AVDexedLibrary::writePackManifest(root.getChildFile("SynprezFM"), "av-dexed-synprezfm", "SynprezFM");

    const auto contents = AVDexedLibrary::describeFolder(root.getChildFile("Pianos"));
    bool passed = expect(contents.banks == 2 && contents.presets == 2 && contents.otherFiles == 2
                             && contents.sounds() == 66 && contents.files() == 6,
                         "a folder's real contents are counted, hidden sounds as other files, system leftovers not at all");

    juce::File renamed;
    passed &= expect(AVDexedLibrary::renameFolder(root.getChildFile("Pianos/Rhodes"), "Wurli", root, &renamed) == RenameResult::renamed
                         && renamed == root.getChildFile("Pianos/Wurli") && renamed.getChildFile("Suitcase.syx").existsAsFile()
                         && ! suitcase.exists(),
                     "a nested folder is renamed on disk with its sounds");
    passed &= expect(AVDexedLibrary::renameFolder(renamed, "Pads", root) == RenameResult::alreadyExists
                         && renamed.getChildFile("Suitcase.syx").existsAsFile() && root.getChildFile("Pianos/Pads").isDirectory(),
                     "a rename onto an existing folder is refused and touches neither");
    passed &= expect(AVDexedLibrary::renameFolder(renamed, " .. ", root) == RenameResult::invalidName, "unusable names are refused");
    passed &= expect(AVDexedLibrary::renameFolder(juce::File(renamed), "WURLI", root, &renamed) == RenameResult::renamed
                         && renamed.getChildFile("Suitcase.syx").existsAsFile()
                         && [&root] {
                                for (const auto& folder : AVDexedLibrary::listFolders(root.getChildFile("Pianos")))
                                    if (folder.getFileName() == "WURLI") return true;
                                return false;
                            }(),
                     "a change of case alone is allowed (and reaches the disk)");
    passed &= expect(AVDexedLibrary::renameFolder(root.getChildFile("SynprezFM"), "Mine", root) == RenameResult::readOnly
                         && root.getChildFile("SynprezFM/SynprezFM_01.syx").existsAsFile(),
                     "packs cannot be renamed");

    juce::File preset;
    passed &= expect(AVDexedLibrary::renameSoundFile(root.getChildFile("Pianos/Mine.dexedpreset"), "Mi sonido", root, &preset) == RenameResult::renamed
                         && preset == root.getChildFile("Pianos/Mi sonido.dexedpreset") && preset.loadFileAsString() == "mine",
                     "a sound file is renamed keeping its extension");
    juce::File second;
    passed &= expect(AVDexedLibrary::renameSoundFile(root.getChildFile("Pianos/Other.dexedpreset"), "Mi sonido", root, &second) == RenameResult::renamed
                         && second == root.getChildFile("Pianos/Mi sonido 2.dexedpreset") && preset.loadFileAsString() == "mine",
                     "a sound renamed onto a taken name gets ' 2' and never replaces the other");
    juce::File same;
    passed &= expect(AVDexedLibrary::renameSoundFile(preset, "Mi sonido", root, &same) == RenameResult::renamed
                         && same.getFullPathName() == preset.getFullPathName() && preset.loadFileAsString() == "mine"
                         && second.loadFileAsString() == "other",
                     "renaming a sound to its own name changes nothing");
    passed &= expect(AVDexedLibrary::renameSoundFile(preset, "mi SONIDO", root, &same) == RenameResult::renamed
                         && same.loadFileAsString() == "mine" && second.loadFileAsString() == "other"
                         && same.getParentDirectory().getChildFile("Mi sonido 2.dexedpreset").existsAsFile(),
                     "a change of case only renames the sound itself, never the other file");
    preset = same;

    const auto removed = AVDexedLibrary::removeToTrash(renamed, root, false);
    passed &= expect(removed.status == AVDexedLibrary::RemoveResult::Status::keptHidden && ! renamed.exists()
                         && removed.hiddenCopy.isAChildOf(root.getChildFile(AVDexedLibrary::deletedFolderName))
                         && removed.hiddenCopy.getFileName().startsWith("WURLI ")
                         && removed.hiddenCopy.getChildFile("Suitcase.syx").existsAsFile(),
                     "a removed folder is moved whole into the hidden .deleted folder, never deleted");
    passed &= expect(! relativeList(AVDexedLibrary::findSoundFiles(root), root).contains("Suitcase"),
                     "removed sounds leave the library");
    const auto removedFile = AVDexedLibrary::removeToTrash(second, root, false);
    passed &= expect(removedFile.status == AVDexedLibrary::RemoveResult::Status::keptHidden && ! second.exists()
                         && removedFile.hiddenCopy.getFileExtension() == ".dexedpreset" && removedFile.hiddenCopy.loadFileAsString() == "other",
                     "a removed sound file keeps its content and extension in .deleted");
    passed &= expect(AVDexedLibrary::removeToTrash(root.getChildFile("SynprezFM"), root, false).status
                         == AVDexedLibrary::RemoveResult::Status::readOnly && root.getChildFile("SynprezFM").isDirectory(),
                     "packs are never removed");
    const auto outside = scratch.root.getSiblingFile("avdexed-library-outside-" + juce::Uuid().toString());
    passed &= expect(AVDexedLibrary::removeToTrash(outside, root, false).status == AVDexedLibrary::RemoveResult::Status::failed
                         && AVDexedLibrary::removeToTrash(root, root, false).status == AVDexedLibrary::RemoveResult::Status::failed,
                     "nothing outside the library (nor the root itself) can be removed");

    passed &= expect(AVDexedLibrary::writableFolderFor(root, "synprezfm") == root.getChildFile("SynprezFM 2")
                         && AVDexedLibrary::writableFolderFor(root, "pianos") == root.getChildFile("Pianos")
                         && AVDexedLibrary::writableFolderFor(root, "pianos").getFileName() == "Pianos"
                         && AVDexedLibrary::writableFolderFor(root, "CON") == juce::File(),
                     "new folders reuse a writable folder of that name, never a pack");
    return passed;
}

bool testLegacyFolders()
{
    const auto folders = AVDexedLibrary::parseLegacyFolders(
        "<PresetFolders><Folder id=\"{a}\" name=\" Pads \"><PresetKey value=\"bank:12ab:03\"/><PresetKey value=\"user:x\"/>"
        "<PresetKey value=\"bank:12ab:03\"/></Folder><Folder id=\"{b}\" name=\"\"/></PresetFolders>");
    return expect(folders.size() == 1 && folders[0].id == "{a}" && folders[0].name == "Pads"
                      && folders[0].keys.joinIntoString(",") == "bank:12ab:03,user:x",
                  "0.1.0 virtual folders are read with their sounds; nameless ones are dropped")
        && expect(AVDexedLibrary::parseLegacyFolders("<PresetFolders/>").empty()
                      && AVDexedLibrary::parseLegacyFolders("not xml").empty(),
                  "an empty or broken catalog gives no folders")
        && expect([&folders] {
                      const auto again = AVDexedLibrary::parseLegacyFolders(AVDexedLibrary::composeLegacyFolders(folders));
                      return again.size() == 1 && again[0].id == "{a}" && again[0].name == "Pads"
                          && again[0].keys == folders[0].keys;
                  }() && AVDexedLibrary::composeLegacyFolders({}).isEmpty(),
                  "what is still to migrate is written back in the same format, and nothing when it is empty");
}

bool testAdoptionKeepsPacksAndPreferences()
{
    ScratchFolder scratch, elsewhere;
    const auto appDirectory = scratch.root;
    writeBankAt(appDirectory, "SynprezFM/SynprezFM_01.syx", 5);
    AVDexedLibrary::writePackManifest(appDirectory.getChildFile("SynprezFM"), "av-dexed-synprezfm", "SynprezFM");
    const auto external = writeBankAt(elsewhere.root, "SynprezFM.syx", 40);
    const auto preferencesFile = appDirectory.getChildFile("AV Dexed Preferences.xml");
    {
        juce::PropertiesFile preferences(preferencesFile, AVDexedLibrary::preferencesOptions());
        preferences.setValue("presetBrowserKnownBanks", external.getFullPathName());
        preferences.setValue("favoritePresetIds", "bank:12ab:03");
        preferences.save();
    }
    const auto copied = AVDexedLibrary::adoptExternalSounds(appDirectory, preferencesFile);
    juce::PropertiesFile after(preferencesFile, AVDexedLibrary::preferencesOptions());
    return expect(copied.size() == 1 && appDirectory.getChildFile("SynprezFM 2/SynprezFM.syx").existsAsFile()
                      && ! appDirectory.getChildFile("SynprezFM/SynprezFM.syx").exists() && external.existsAsFile(),
                  "a bank adopted from outside never lands in a read-only pack of the same name")
        && expect(after.getValue("favoritePresetIds") == "bank:12ab:03" && after.getValue("presetBrowserKnownBanks").isEmpty(),
                  "adoption updates only its own keys of the preferences");
}

juce::MemoryBlock fileBytes(const juce::File& file)
{
    juce::MemoryBlock bytes;
    file.loadFileAsData(bytes);
    return bytes;
}

/** Files of the folder other than `keep`: a failed write must leave no temporary file behind. */
int strayFiles(const juce::File& folder, const juce::File& keep)
{
    int count = 0;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findFiles))
        if (entry.getFile() != keep) ++count;
    return count;
}

bool testSafeWrites()
{
    ScratchFolder scratch;
    const auto file = scratch.root.getChildFile("Bank.syx");
    const auto bankA = makeBank(0), bankB = makeBank(30);
    bool passed = expect(AVDexedLibrary::writeAtomically(file, bankA) && fileBytes(file) == bankA
                             && AVDexedLibrary::writeAtomically(file, bankB) && fileBytes(file) == bankB
                             && strayFiles(scratch.root, file) == 0,
                         "an atomic write creates or replaces the whole file and leaves no temporary file");
    passed &= expect(! AVDexedLibrary::writeAtomically(file, {}) && fileBytes(file) == bankB,
                     "empty data is refused and never replaces a file with nothing");
    passed &= expect(! AVDexedLibrary::writeAtomically(scratch.root.getChildFile("Missing/New.syx"), bankA)
                         && ! scratch.root.getChildFile("Missing").exists(),
                     "a write that cannot be made reports failure and creates nothing");

    // A folder that refuses new files (as a full disk refuses the data): the
    // temporary file cannot be written, so the bank in it stays as it was.
    const auto locked = scratch.root.getChildFile("Locked");
    const auto lockedFile = locked.getChildFile("Bank.syx");
    locked.createDirectory();
    lockedFile.replaceWithData(bankA.getData(), bankA.getSize());
    if (locked.setReadOnly(true) && ! locked.hasWriteAccess()) {
        const bool refused = ! AVDexedLibrary::writeAtomically(lockedFile, bankB);
        locked.setReadOnly(false);
        passed &= expect(refused && fileBytes(lockedFile) == bankA && strayFiles(locked, lockedFile) == 0,
                         "a write that fails keeps the original file untouched");
    }
    locked.setReadOnly(false);

    // Cartridge::saveVoice (Store, a voice dragged onto a bank) goes through the same write.
    Cartridge edited;
    edited.load(static_cast<const uint8_t*>(bankB.getData()), static_cast<int>(bankB.getSize()));
    const auto fresh = scratch.root.getChildFile("Fresh.syx");
    passed &= expect(edited.saveVoice(fresh) == 1 && fresh.getSize() == SYSEX_SIZE,
                     "saving a bank into a new file writes the 32 voices");

    // A bank file with its settings: only the bank message is replaced.
    Cartridge original;
    original.load(static_cast<const uint8_t*>(bankA.getData()), static_cast<int>(bankA.getSize()));
    AVDexedLibrary::BankSettings settings;
    settings[4] = { AVDexedLibrary::voiceHash(original, 4), { { "reverbAmount", 0.5f } } };
    const auto withSettings = scratch.root.getChildFile("With settings.syx");
    const auto composed = AVDexedLibrary::composeBankFile({}, original, settings);
    withSettings.replaceWithData(composed.getData(), composed.getSize());
    Cartridge reread;
    const bool patched = edited.saveVoice(withSettings) == 1
        && AVDexedLibrary::readBank(withSettings, reread)
        && AVDexedLibrary::bankFingerprint(reread) == AVDexedLibrary::bankFingerprint(edited)
        && AVDexedLibrary::readEmbeddedSettings(withSettings).count(4) == 1
        && withSettings.getSize() == static_cast<juce::int64>(composed.getSize());
    passed &= expect(patched, "saving into an existing bank file replaces only its bank and keeps its sound settings");

    // A multi-bank archive (2 MB of concatenated banks): only its first 64 KB
    // are ever read, so rewriting it would cut off everything after them.
    const auto archive = scratch.root.getChildFile("DX7 Collection.syx");
    juce::MemoryBlock collection;
    for (int bank = 0; bank < 500; ++bank) {
        const auto bytes = makeBank(bank);
        collection.append(bytes.getData(), bytes.getSize());
    }
    archive.replaceWithData(collection.getData(), collection.getSize());
    passed &= expect(edited.saveVoice(archive) == -1 && fileBytes(archive) == collection,
                     "a bank file of 64 KB or more is never rewritten (no truncated archive)");

    // The limit itself: a bank followed by other messages up to exactly 65535 bytes.
    const auto atLimit = scratch.root.getChildFile("At limit.syx");
    juce::MemoryBlock limitBytes(bankA);
    juce::MemoryBlock filler(static_cast<size_t>(Cartridge::maxRewritableFileSize) - bankA.getSize(), true);
    auto* fill = static_cast<uint8_t*>(filler.getData());
    fill[0] = 0xf0;
    fill[1] = 0x7d;
    fill[filler.getSize() - 1] = 0xf7;
    limitBytes.append(filler.getData(), filler.getSize());
    atLimit.replaceWithData(limitBytes.getData(), limitBytes.getSize());
    passed &= expect(atLimit.getSize() == Cartridge::maxRewritableFileSize
                         && edited.saveVoice(atLimit) == -1 && fileBytes(atLimit) == limitBytes,
                     "a file at the 65535-byte limit is left as it was");
    return passed;
}

/** Windows: another program (an antivirus, the search indexer) holds a file
    for a moment, longer than JUCE's own tries of a swap (about 0.4 s). A save
    over it and a move of it wait; held for good, the save fails and leaves
    the file as it was. */
bool testFilesHeldForAMoment()
{
   #if JUCE_WINDOWS
    ScratchFolder scratch;
    const auto file = scratch.root.getChildFile("Bank.syx");
    const auto bankA = makeBank(0), bankB = makeBank(30);
    file.replaceWithData(bankA.getData(), bankA.getSize());
    // Held as a scanner holds it: others may read, never rename or delete.
    const auto hold = [](const juce::File& held) {
        return CreateFileW(held.getFullPathName().toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    const auto releaseLater = [](HANDLE handle) {
        return std::thread([handle] {
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            CloseHandle(handle);
        });
    };
    auto handle = hold(file);
    bool passed = expect(handle != INVALID_HANDLE_VALUE, "a test file can be held open");
    auto release = releaseLater(handle);
    passed &= expect(AVDexedLibrary::writeAtomically(file, bankB) && fileBytes(file) == bankB
                         && strayFiles(scratch.root, file) == 0,
                     "a save over a file held for a moment waits for it and leaves nothing behind");
    release.join();

    const auto moved = scratch.root.getChildFile("Moved.syx");
    release = releaseLater(hold(file));
    passed &= expect(AVDexedLibrary::moveFile(file, moved) && moved.existsAsFile() && ! file.exists(),
                     "a move of a file held for a moment waits for it");
    release.join();

    handle = hold(moved);
    passed &= expect(! AVDexedLibrary::writeAtomically(moved, bankA) && fileBytes(moved) == bankB
                         && strayFiles(scratch.root, moved) == 0,
                     "a save over a file held for good fails and leaves the file as it was");
    CloseHandle(handle);
    return passed;
   #else
    return true;
   #endif
}

bool testMigrationNeverFillsPacks()
{
    ScratchFolder scratch;
    const auto appDirectory = scratch.root;
    // Already migrated: the factory folders are read-only packs.
    writeBankAt(appDirectory, "SynprezFM/SynprezFM_01.syx", 5);
    AVDexedLibrary::writePackManifest(appDirectory.getChildFile("SynprezFM"), "av-dexed-synprezfm", "SynprezFM");
    const auto factoryBank = writeBankAt(appDirectory, "Dexed Factory/Dexed_01.syx", 1);
    AVDexedLibrary::writePackManifest(appDirectory.getChildFile("Dexed Factory"), "av-dexed-factory", "Dexed Factory");
    appDirectory.getChildFile(".library-layout-v2").replaceWithText("completed");
    const auto factoryBytes = fileBytes(factoryBank);

    // A 0.1.0 build recreated Cartridges/ and the user saved banks there.
    writeBankAt(appDirectory, "Cartridges/SynprezFM/SynprezFM_01.syx", 5);   // the factory copy, unchanged
    const auto mine = fileBytes(writeBankAt(appDirectory, "Cartridges/SynprezFM/Mine.syx", 50));
    const auto edited = fileBytes(writeBankAt(appDirectory, "Cartridges/Dexed_01.syx", 77));

    const bool migrated = AVDexedLibrary::migrateLayout(appDirectory, nullptr, 0);
    juce::Array<juce::File> packFiles;
    appDirectory.getChildFile("SynprezFM").findChildFiles(packFiles, juce::File::findFiles, true);
    juce::Array<juce::File> factoryFiles;
    appDirectory.getChildFile("Dexed Factory").findChildFiles(factoryFiles, juce::File::findFiles, true);
    juce::Array<juce::File> archived;
    appDirectory.getChildFile(".migration-backup").findChildFiles(archived, juce::File::findFiles, true, "SynprezFM_01*.syx");

    return expect(migrated && ! appDirectory.getChildFile("Cartridges").exists(), "the recreated Cartridges/ is absorbed again")
        && expect(packFiles.size() == 2 && factoryFiles.size() == 2 && fileBytes(factoryBank) == factoryBytes,
                  "nothing new or changed is moved into a read-only pack")
        && expect(fileBytes(appDirectory.getChildFile("SynprezFM 2/Mine.syx")) == mine
                      && fileBytes(appDirectory.getChildFile("Dexed Factory 2/Dexed_01.syx")) == edited,
                  "the user's new and edited banks go to a writable twin of the pack")
        && expect(archived.size() == 1, "a file identical to the pack's own is archived, as before");
}
}

bool testPreferencesRename()
{
    ScratchFolder scratch;
    const auto oldFile = scratch.root.getChildFile("Dexed.xml");
    const auto newFile = scratch.root.getChildFile("AV Dexed Preferences.xml");
    oldFile.replaceWithText("<PROPERTIES><VALUE name=\"avDexedLanguage\" val=\"es\"/></PROPERTIES>");
    bool passed = expect(AVDexedLibrary::migratePreferences(scratch.root, "AV Dexed Preferences.xml"), "the rename succeeds");
    passed &= expect(newFile.loadFileAsString().contains("avDexedLanguage") && ! oldFile.exists(),
                     "the preferences move to the new name and the old name disappears from the folder");
    juce::Array<juce::File> backups;
    scratch.root.getChildFile(".migration-backup").findChildFiles(backups, juce::File::findFiles, false, "Dexed *.xml");
    passed &= expect(backups.size() == 1, "the old file is kept in the hidden backup");
    oldFile.replaceWithText("<PROPERTIES><VALUE name=\"avDexedLanguage\" val=\"en\"/><VALUE name=\"zoom\" val=\"2\"/></PROPERTIES>");
    passed &= expect(AVDexedLibrary::migratePreferences(scratch.root, "AV Dexed Preferences.xml")
                         && newFile.loadFileAsString().contains("val=\"es\"") && newFile.loadFileAsString().contains("zoom"),
                     "a later old file is merged key by key; keys already there win");
    passed &= expect(AVDexedLibrary::migratePreferences(scratch.root, "AV Dexed Preferences.xml"), "running it again is harmless");
    return passed;
}

bool testPresetExtension()
{
    ScratchFolder scratch;
    const auto root = AVDexedLibrary::rootFor(scratch.root);
    root.getChildFile("Pianos").createDirectory();
    root.getChildFile("Pianos/Luis.dexedpreset").replaceWithText("luis");
    root.getChildFile("Pianos/Clash.dexedpreset").replaceWithText("old");
    root.getChildFile("Pianos/Clash.avdexpreset").replaceWithText("new");
    root.getChildFile(".deleted").createDirectory();
    root.getChildFile(".deleted/Gone.dexedpreset").replaceWithText("gone");
    bool passed = expect(AVDexedLibrary::renameLegacyPresets(root), "old sound files are renamed");
    passed &= expect(root.getChildFile("Pianos/Luis.avdexpreset").loadFileAsString() == "luis"
                         && ! root.getChildFile("Pianos/Luis.dexedpreset").exists(),
                     "a .dexedpreset becomes .avdexpreset with the same content");
    passed &= expect(root.getChildFile("Pianos/Clash.avdexpreset").loadFileAsString() == "new"
                         && root.getChildFile("Pianos/Clash 2.avdexpreset").loadFileAsString() == "old",
                     "a rename never replaces a file that already has the new name");
    passed &= expect(root.getChildFile(".deleted/Gone.dexedpreset").existsAsFile(), "hidden folders keep their files as they are");
    root.getChildFile("Pianos/Late.dexedpreset").replaceWithText("late");
    passed &= expect(AVDexedLibrary::renameLegacyPresets(root) && root.getChildFile("Pianos/Late.dexedpreset").existsAsFile(),
                     "the rename runs once (later old files still open under their name)");
    passed &= expect(AVDexedLibrary::isPresetFile(root.getChildFile("Pianos/Late.dexedpreset"))
                         && AVDexedLibrary::isPresetFile(root.getChildFile("x.AVDEXPRESET"))
                         && ! AVDexedLibrary::isPresetFile(root.getChildFile("x.syx")),
                     "both names are sound files, in any case");
    passed &= expect(AVDexedLibrary::remapPresetPath(root.getChildFile("Pianos/Luis.dexedpreset")) == root.getChildFile("Pianos/Luis.avdexpreset")
                         && AVDexedLibrary::remapPresetPath(root.getChildFile("Pianos/Late.dexedpreset")) == root.getChildFile("Pianos/Late.dexedpreset"),
                     "a session path to an old name finds the renamed file, and an existing old file stays");
    const auto outside = scratch.root.getChildFile("Downloads");
    outside.createDirectory();
    outside.getChildFile("Shared.dexedpreset").replaceWithText("shared");
    const auto imported = AVDexedLibrary::importPreset(outside.getChildFile("Shared.dexedpreset"), root.getChildFile("Pianos"));
    passed &= expect(imported.status == AVDexedLibrary::ImportResult::Status::copied
                         && imported.file == root.getChildFile("Pianos/Shared.avdexpreset") && imported.file.loadFileAsString() == "shared",
                     "an imported .dexedpreset arrives under the current name");
    const auto again = AVDexedLibrary::importPreset(outside.getChildFile("Shared.dexedpreset"), root.getChildFile("Pianos"));
    passed &= expect(again.status == AVDexedLibrary::ImportResult::Status::alreadyThere && again.file == imported.file,
                     "importing it again finds the renamed copy");

    // Two symbolic links back to the root must not make the walk endless.
    ScratchFolder looped;
    const auto loopRoot = AVDexedLibrary::rootFor(looped.root);
    loopRoot.getChildFile("A").createDirectory();
    loopRoot.getChildFile("B").createDirectory();
    loopRoot.getChildFile("A/Sound.dexedpreset").replaceWithText("a");
    const bool linked = loopRoot.createSymbolicLink(loopRoot.getChildFile("A/Loop"), true)
                     && loopRoot.createSymbolicLink(loopRoot.getChildFile("B/Loop"), true);
    if (linked)
        passed &= expect(AVDexedLibrary::renameLegacyPresets(loopRoot) && loopRoot.getChildFile(".preset-extension-v1").existsAsFile()
                             && loopRoot.getChildFile("A/Sound.avdexpreset").loadFileAsString() == "a",
                         "links that loop back into the library do not stop or repeat the rename");
    return passed;
}

bool runAVDexedLibraryTests()
{
    return testPreferencesRename()
        && testFolderNames()
        && testImportCopiesWithoutOverwriting()
        && testRealFolders()
        && testBrowserScan()
        && testNameOrder()
        && testLibrarySignature()
        && testFolderOperations()
        && testLegacyFolders()
        && testAdoptionKeepsPacksAndPreferences()
        && testSafeWrites()
        && testFilesHeldForAMoment()
        && testMigrationNeverFillsPacks()
        && testPresetExtension();
}
