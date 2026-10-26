// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <map>
#include <vector>
#include "Cartridge.h"

/** AV Dexed's sound library on disk. The data folder itself
    (`~/Library/Application Support/AV Vibes/AV Dexed` on macOS) is the library:
    every folder in it is a folder in the plugin, 1:1 with Finder or Explorer,
    and holds .syx banks (32 voices each) and .avdexpreset sounds. The modern
    settings of each bank voice (volume, pan, cutoff, reverb, delay, chorus) are
    embedded in the .syx itself, so they travel with the file.

    Nothing in here touches the processor or the UI, so all of it is tested on
    temporary folders. Every write that can race with another AV Dexed (two DAWs,
    a plugin scan, a second instance in the same DAW) holds a LibraryScope. */
namespace AVDexedLibrary {

inline constexpr const char* factoryFolderName = "Dexed Factory";
inline constexpr const char* synprezFolderName = "SynprezFM";
inline constexpr const char* userPresetsFolderName = "User Presets";
/** Hidden manifest of a pack (factory sounds, expansions). Packs are read-only. */
inline constexpr const char* packManifestName = ".avdexed-pack.json";
inline constexpr const char* profilesFolderName = ".profiles";
inline constexpr const char* backupFolderName = ".migration-backup";
/** Where the original .syx files of imported banks are kept, once AV Dexed has
    turned their voices into sound files. Never listed, never read as sounds. */
inline constexpr const char* syxBackupsFolderName = "UserSYXsBackups";
/** AV Dexed's own sound file (one sound with all its settings). */
inline constexpr const char* presetExtension = ".avdexpreset";
/** The same format as it was named until 0.2.0; still read everywhere. */
inline constexpr const char* legacyPresetExtension = ".dexedpreset";

/** A sound file under either name (any case). */
bool isPresetFile(const juce::File& file);
/** Wildcards for file choosers: both preset names. */
inline constexpr const char* presetWildcards = "*.avdexpreset;*.dexedpreset";

/** Gives the library's .dexedpreset files their current name (.avdexpreset),
    once (marker `.preset-extension-v1`). Never over an existing file: a clash
    keeps " 2". Returns false when something could not be renamed (retried on
    the next launch). */
bool renameLegacyPresets(const juce::File& root);
/** A preset path saved before the rename (a DAW session), mapped to its new
    name when only that one exists. */
juce::File remapPresetPath(const juce::File& file);

/** The library root: the data folder itself. */
juce::File rootFor(const juce::File& appDirectory);

//==============================================================================
/** The browser's content identity (kept stable since 0.1.0: favourites depend on it). */
juce::String stableHash(const void* data, size_t size);
juce::uint64 stableHash64(const void* data, size_t size);
/** Fingerprint of a bank's 32 voices (the `bank:<fingerprint>:NN` keys). */
juce::String bankFingerprint(const Cartridge& bank);
/** Identity of one packed voice (128 bytes), used to keep settings on their sound. */
juce::uint64 voiceHash(const Cartridge& bank, int slot);

/** Hidden, system, temporary or reserved names: never shown as folders or sounds. */
bool isIgnoredName(const juce::String& name);

/** A safe, portable folder name (empty when nothing usable is left): trimmed,
    no path or reserved characters, no leading/trailing dots or spaces, not a
    Windows device name, not a name the library hides, at most 48 characters. */
juce::String sanitizeFolderName(const juce::String& requested);

/** Folders of the library, sorted case-insensitively. With `nested`, subfolders
    follow their parent ("Pianos", "Pianos/Rhodes"). */
juce::Array<juce::File> listFolders(const juce::File& root, bool nested = false);

/** The folder a file lives in, relative to the root with '/' ("Pianos/Rhodes"),
    or "" for files loose in the root. */
juce::String folderOf(const juce::File& file, const juce::File& root);

/** An existing first-level folder whose name matches ignoring case (keeping
    its spelling on disk), or `root/name`. */
juce::File folderNamed(const juce::File& root, const juce::String& name);

/** Never overwrites: `name.syx`, then `name 2.syx`, `name 3.syx`… */
juce::File uniqueDestination(const juce::File& folder, const juce::String& fileName);

/** Loads a bank file the way the library does (< 64 KB, a DX7 32-voice SysEx,
    checksum errors tolerated). Returns false for anything else. */
bool readBank(const juce::File& file, Cartridge& bank, int* loadResult = nullptr);

/** The bank to install for `file`: what the file holds now when it can be read,
    else `snapshot`. A snapshot taken before a save (the browser's index, the
    bank manager's preview, a bank read before a "save changes?" prompt) can be
    older than the file; loading it would undo the save in memory. */
Cartridge currentBankOf(const juce::File& file, const Cartridge& snapshot);

/** True when the file or folder is inside a pack (a folder with the manifest). */
bool isReadOnly(const juce::File& fileOrFolder, const juce::File& root);
bool writePackManifest(const juce::File& folder, const juce::String& id, const juce::String& name);

//==============================================================================
/** Cross-process locks (re-entrant within one process). */
juce::InterProcessLock& libraryLock();
juce::InterProcessLock& preferencesLock();

/** Holds libraryLock() for a scope together with a process-wide mutex. An
    InterProcessLock only keeps other processes out: inside one process it just
    counts, and two AV Dexed instances in one DAW (or a background copy) share a
    process. Re-entrant on one thread. `held` is false when it was not free in time. */
class LibraryScope {
public:
    explicit LibraryScope(int timeoutMs = 4000);
    ~LibraryScope();
    const bool held;
    JUCE_DECLARE_NON_COPYABLE(LibraryScope)
};
/** Options for the preferences file: XML, guarded by preferencesLock(). */
juce::PropertiesFile::Options preferencesOptions();

/** Writes through a hidden temporary sibling, then swaps it in, only once the
    whole block is on disk (write, flush and size checked): a full disk leaves
    the original untouched. Empty data is refused (false). */
bool writeAtomically(const juce::File& file, const juce::MemoryBlock& data);

//==============================================================================
/** Modern settings of one voice, in real parameter units (as in session state).
    Only values that differ from the default are stored. */
struct SlotSettings {
    juce::uint64 voice = 0;
    std::map<juce::String, float> values;
};
using BankSettings = std::map<int, SlotSettings>;   // key: slot 0–31

/** The settings embedded in a .syx (an extra non-commercial SysEx message
    `F0 7D 'AVDX' 01 <JSON> F7` after the bank; a DX7 ignores it). */
BankSettings readEmbeddedSettings(const juce::MemoryBlock& fileBytes);
BankSettings readEmbeddedSettings(const juce::File& bank);
/** The file bytes with `bank` as its DX7 bank and `settings` embedded; every
    other SysEx message of the original file is kept in place. */
juce::MemoryBlock composeBankFile(const juce::MemoryBlock& originalBytes, Cartridge bank, const BankSettings& settings);

//==============================================================================
/** What a folder already holds, by content: a batch import reads the folder
    once instead of once per imported file. Pass the same index for every file
    going into the same folder. */
struct FolderIndex {
    juce::File folder;
    std::multimap<juce::uint64, juce::File> files;   // hash of the whole file → file
    bool built = false;
};

struct ImportResult {
    enum class Status { copied, alreadyThere, notABank, failed };
    Status status = Status::failed;
    juce::File file;   // the file inside the library (copied or already there): the first one
    juce::Array<juce::File> files;   // every file the import produced or reused (banks split from one file)
    int sounds = 0;    // sounds added by the copies (32 per bank; the real voices of a bank built from single voices)
};

/** Largest file an import reads (archives of many banks). */
inline constexpr juce::int64 maxImportBytes = 16 * 1024 * 1024;

/** Copies a bank into `folder` (created if needed). A file already in that
    folder, or identical content there under any name, is reused; a different
    file with the same name gets " 2". Never overwrites.
    A standard bank file (one bank, complete messages, under 64 KB) is copied as
    it is. Anything else of the DX7 family becomes standard banks, one file per
    bank ("name A"/"name B", or "name 01"… for more): several banks in one file
    (DX7II/TX802 64-voice dumps, archives larger than 64 KB), a cartridge saved
    without its SysEx header, messages after a librarian's header, the last F7
    missing, single voices (their voices in order, the rest of the bank Init).
    The DX7II/TX802 additional data (AMEM) sent with a bank stays with it. */
ImportResult importBank(const juce::File& source, const juce::File& folder, FolderIndex* index = nullptr);
/** True for a file of single DX7 voices (VCED) without any bank: the import
    gathers those of one folder into banks of 32 (importVoices). */
bool isSingleVoiceFile(const juce::File& file);
/** The voices of `sources` (in file-name order) as banks of 32 in `folder`,
    named `bankName` ("bankName 1", "bankName 2"… for more than one bank). */
ImportResult importVoices(const juce::Array<juce::File>& sources, const juce::File& folder,
                          const juce::String& bankName, FolderIndex* index = nullptr);
/** Same for an .avdexpreset (or .dexedpreset) sound (no content check beyond its size). */
ImportResult importPreset(const juce::File& source, const juce::File& folder, FolderIndex* index = nullptr);

/** One bank found in a file, as the library would play it. */
struct SourceBank {
    Cartridge bank;
    BankSettings settings;   // embedded in a standard bank file; none for a bank taken out of a dump
    juce::String name;       // the file's name ("Rhodes"), numbered when the file holds several ("Archivo 2")
    int voices = 32;         // a bank built from single voices: only its first `voices` are real
    bool singleVoices = false;   // built from single voices (VCED), not a bank of the file
};
/** The banks of a file of the DX7 family, the way importBank reads them: a
    standard bank file as it is (with its settings and AMEM), every distinct
    bank of a dump or archive, single voices gathered in banks of 32. Empty
    when it holds no voice (a performance file, something else). */
std::vector<SourceBank> readSourceBanks(const juce::File& file);

//==============================================================================
/** Fingerprints of every bank in the library. */
juce::StringArray libraryFingerprints(const juce::File& root);

/** Writes the factory banks: Dexed_01 into "Dexed Factory", the SynprezFM banks
    into "SynprezFM", each folder marked as a read-only pack when it is created.
    With `restoreMissing`, only banks whose content is nowhere in the library are
    written (" 2" if the name is taken by a different bank). Returns the count. */
int installFactorySounds(const juce::File& root, const void* zipData, size_t zipSize, bool restoreMissing);

/** 0.1.0 preferences ("Dexed.xml") are merged into `newName` key by key (keys
    already there win; favourites are joined), then the old file is archived in
    `.migration-backup/`. Safe to run any number of times and in parallel. */
bool migratePreferences(const juce::File& appDirectory, const juce::String& newName);

/** 0.1.0 layout → real folders: `Cartridges/` is absorbed into the data folder
    (its subfolders become library folders; Dexed_01 goes to "Dexed Factory"),
    `Profiles/` becomes hidden, factory folders identical to the originals are
    marked read-only, and a first install gets the factory sounds. Nothing is
    deleted: duplicates and leftovers go to `.migration-backup/`. Idempotent; it
    also cleans up after a 0.1.0 build that recreated `Cartridges/`, and then a
    new or changed file never lands in a read-only pack: it goes to the pack's
    writable twin ("SynprezFM 2", "Dexed Factory 2"). */
bool migrateLayout(const juce::File& appDirectory, const void* factoryZip, size_t factoryZipSize);

/** A path saved by an older version that pointed into the 0.1.0 layout, mapped
    to where the migration put it (or the original when nothing matches). */
juce::File remapLegacyPath(const juce::File& file, const juce::File& appDirectory);

/** Banks and presets that 0.1.0 played from outside the data folder (by path)
    are copied into the library. Unreachable ones (e.g. an unmounted drive) stay
    listed and are retried later. Returns the names copied now. */
juce::StringArray adoptExternalSounds(const juce::File& appDirectory, const juce::File& preferencesFile);

/** adoptExternalSounds in two halves, so the copying can run on a background
    thread while the preferences are only written on the message thread (where
    every other preferences write of this process happens). */
struct ExternalAdoption {
    juce::StringArray copied;                       // names copied now
    juce::StringArray settledBanks, settledPresets; // paths that no longer need adopting
};
ExternalAdoption copyExternalSounds(const juce::File& appDirectory, const juce::File& preferencesFile);
/** Removes the settled paths from the pending lists (re-read, merge, save). */
void settleExternalSounds(const juce::File& preferencesFile, const ExternalAdoption& adoption);

//==============================================================================
// The Play view's browser on the real folders.

/** Hidden folder of the root where removed folders and sounds wait for the
    system trash (and stay, if the trash refuses them). */
inline constexpr const char* deletedFolderName = ".deleted";

/** A .syx bank or an .avdexpreset / .dexedpreset sound (extension in any case) with a visible name. */
bool isSoundFile(const juce::File& file);

/** Every sound file of the library at any depth: the root's loose files first,
    then each folder (parents before children, as listFolders(root, true)), names
    sorted naturally. The same names are skipped as in listFolders(), at every
    level; a symbolic link back into the library is not entered twice. */
juce::Array<juce::File> findSoundFiles(const juce::File& root);
/** A sound file with its size and modification time (milliseconds, as
    juce::File::getLastModificationTime() gives it). */
struct SoundFileInfo {
    juce::File file;
    juce::int64 size = 0, modified = 0;
};
/** findSoundFiles() with each file's size and time, asking the disk once per
    file: the browser's walk of a library of tens of thousands of sounds. */
std::vector<SoundFileInfo> listSoundFiles(const juce::File& root);

/** Hidden folder of the root for what AV Dexed keeps about the library (the
    browser's index, where converted bank voices went). Never listed. */
inline constexpr const char* cacheFolderName = ".cache";
/** The browser's index of the library's sound files, kept between windows
    and sessions: what each file shows for its size and time, so a new window
    reads only the files that changed. */
struct IndexedSound {
    juce::String path;                     // below the root, with '/'
    juce::int64 size = 0, modified = 0;    // as SoundFileInfo
    int order = -1;                        // a converted bank voice's place in its folder
    juce::String key, name;                // the browser's identity ("user:<id>") and the name shown
};
std::map<juce::String, IndexedSound> readSoundIndex(const juce::File& root);   // by path
bool writeSoundIndex(const juce::File& root, const std::vector<IndexedSound>& sounds);
/** Whether the library's own banks have been turned into sound files. */
bool isLibraryConverted(const juce::File& root);
void setLibraryConverted(const juce::File& root, bool converted);

/** A cheap fingerprint of the library's shape for polling: relative path and
    modification time of every folder at any depth, plus name, size and time of
    the sound files loose in the root. No file is opened. The root's own time is
    left out on purpose: the preferences file is rewritten there (favourites,
    selected folder), and that must not look like a library change. */
juce::String librarySignature(const juce::File& root);

/** What a folder really holds on disk, for an honest delete confirmation. */
struct FolderContents {
    int banks = 0;        // .syx files that load as a DX7 bank (32 sounds each)
    int presets = 0;      // .avdexpreset sounds (or .dexedpreset)
    int otherFiles = 0;   // anything else (hidden folders included) that AV Dexed does not show
    int files() const noexcept { return banks + presets + otherFiles; }
    int sounds() const noexcept { return banks * 32 + presets; }
};
/** Counts every file below `folder` except system leftovers (.DS_Store, Thumbs.db…). */
FolderContents describeFolder(const juce::File& folder);

enum class RenameResult { renamed, invalidName, alreadyExists, readOnly, notFound, failed };
/** Renames a library folder (at any depth) in place, holding the library lock.
    The name goes through sanitizeFolderName(); an existing folder or file with
    that name is never replaced (alreadyExists), but a change of case alone is
    allowed. Packs and folders inside them cannot be renamed. */
RenameResult renameFolder(const juce::File& folder, const juce::String& requestedName,
                          const juce::File& root, juce::File* renamedTo = nullptr);
/** Renames a sound file keeping its extension (same name rules as folders). If
    another file took the name meanwhile, the new name gets " 2": nothing is ever
    replaced. Not for files inside packs. */
RenameResult renameSoundFile(const juce::File& file, const juce::String& requestedStem,
                             const juce::File& root, juce::File* renamedTo = nullptr);

struct RemoveResult {
    enum class Status { trashed, keptHidden, readOnly, failed };
    Status status = Status::failed;
    juce::File hiddenCopy;   // its place inside `.deleted` (still there when keptHidden)
};
/** Removes a library folder or sound without ever deleting anything itself: it
    is moved to `<root>/.deleted/<name> <date time>` (a rename on the same disk,
    under the library lock), then handed to the system trash. If the trash
    refuses it, it simply stays hidden in `.deleted`. On Windows it is only
    handed over when the Recycle Bin really keeps it (never deleted for good,
    as Windows does with what it cannot recycle). Packs are refused.
    Tests pass `useSystemTrash = false` so nothing reaches the user's trash. */
RemoveResult removeToTrash(const juce::File& item, const juce::File& root, bool useSystemTrash = true);

/** The virtual folders of 0.1.0 (preference "presetBrowserFolders"): a name and
    the content keys of its sounds. Folders without id or name are dropped. */
struct LegacyFolder {
    juce::String id, name;
    juce::StringArray keys;
};
std::vector<LegacyFolder> parseLegacyFolders(const juce::String& xml);
/** The same catalog written back (what is still to migrate); "" when empty. */
juce::String composeLegacyFolders(const std::vector<LegacyFolder>& folders);

/** The first-level folder to use for `requestedName`: an existing writable
    folder with that name (any case, keeping its spelling), else a new path. A
    pack (read-only) is never reused: "Name 2" instead. File() for names that
    sanitizeFolderName() refuses. Nothing is created. */
juce::File writableFolderFor(const juce::File& root, const juce::String& requestedName);

}
