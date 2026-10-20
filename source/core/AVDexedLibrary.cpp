// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedLibrary.h"
#include "AVDexedSysex.h"
#include "Program.h"
#include <set>

#if JUCE_WINDOWS
 // removeToTrash() hands items to the Recycle Bin through IFileOperation, which
 // can refuse a permanent delete; JUCE's File::moveToTrash() cannot.
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <shellapi.h>
 #include <shlobj.h>
 #include <shobjidl.h>
 #if defined (_MSC_VER)
  #pragma comment (lib, "ole32.lib")
  #pragma comment (lib, "shell32.lib")
  #pragma comment (lib, "uuid.lib")
 #endif
#endif

using namespace juce;

namespace AVDexedLibrary {

namespace {
// The embedded settings message: F0 7D 'A' 'V' 'D' 'X' 01 <ASCII JSON> F7.
const uint8 settingsHeader[] { 0xf0, 0x7d, 'A', 'V', 'D', 'X', 0x01 };
constexpr int lockTimeoutMs = 4000;

bool sameContent(const File& a, const File& b) {
    if (a.getSize() != b.getSize()) return false;
    MemoryBlock left, right;
    return a.loadFileAsData(left) && b.loadFileAsData(right) && left == right;
}

bool isBankFile(const File& file) {
    return file.getFileExtension().equalsIgnoreCase(".syx") && ! isIgnoredName(file.getFileName());
}

bool isReservedFolderName(const String& name) {
    // Folders of the data folder that are not sound folders.
    return name.equalsIgnoreCase("Cartridges") || name.equalsIgnoreCase("Profiles");
}

CriticalSection& libraryMutex() { static CriticalSection mutex; return mutex; }

bool enterLibrary(int timeoutMs) {
    const auto deadline = Time::getMillisecondCounter() + static_cast<uint32>(jmax(0, timeoutMs));
    while (! libraryMutex().tryEnter()) {
        if (Time::getMillisecondCounter() >= deadline) return false;
        Thread::sleep(2);
    }
    if (libraryLock().enter(timeoutMs)) return true;
    libraryMutex().exit();
    return false;
}

/** Holds a cross-process lock for a scope; `held` is false on timeout. */
struct ScopedLibraryLock {
    explicit ScopedLibraryLock(InterProcessLock& lockToUse) : lock(lockToUse), held(lockToUse.enter(lockTimeoutMs)) {}
    ~ScopedLibraryLock() { if (held) lock.exit(); }
    InterProcessLock& lock;
    const bool held;
};

/** Moves a file into the backup folder under a free name. */
bool archive(const File& file, const File& backupFolder) {
    if (! backupFolder.createDirectory().wasOk()) return false;
    return file.moveFileTo(uniqueDestination(backupFolder, file.getFileName()));
}

/** Moves `source` to `destination` without ever replacing anything: identical
    content already there → the source is archived; different → " 2". */
bool mergeFile(const File& source, const File& destination, const File& backupFolder) {
    if (! destination.getParentDirectory().createDirectory().wasOk()) return false;
    if (! destination.exists()) return source.moveFileTo(destination);
    if (sameContent(source, destination)) return archive(source, backupFolder);
    return source.moveFileTo(uniqueDestination(destination.getParentDirectory(), destination.getFileName()));
}

/** Where the migration puts a file of the 0.1.0 layout. A read-only pack only
    takes what it already holds (mergeFile() archives that copy); a new or
    different file is a bank the user made or edited (a 0.1.0 build recreated
    Cartridges/ after the packs existed), so it goes to `writable`, the same
    place in a writable twin ("SynprezFM 2"), where it can still be saved into,
    renamed and removed. */
File packSafeDestination(const File& source, const File& destination, const File& writable, const File& root) {
    if (writable == File() || ! isReadOnly(destination, root)) return destination;
    if (destination.existsAsFile() && sameContent(source, destination)) return destination;
    return writable;
}

bool isLeftover(const String& name) {
    return name == ".DS_Store" || name.equalsIgnoreCase("Thumbs.db") || name.equalsIgnoreCase("desktop.ini")
        || name.startsWith("._");
}

/** Merges every file of `source` into `destination` (same relative paths), then
    removes the folders it emptied. Leftover system files are archived. With a
    `writable` twin, what `destination` (a pack) does not already hold goes
    there instead (see packSafeDestination()). */
bool mergeFolder(const File& source, const File& destination, const File& backupFolder,
                 const File& writable = {}, const File& root = {}) {
    bool ok = true;
    Array<File> files;
    for (const auto& entry : RangedDirectoryIterator(source, true, "*", File::findFiles))
        files.add(entry.getFile());
    for (const auto& file : files) {
        const auto relative = file.getRelativePathFrom(source);
        if (isLeftover(file.getFileName())) {
            ok &= archive(file, backupFolder.getChildFile(relative).getParentDirectory());
            continue;
        }
        const auto target = packSafeDestination(file, destination.getChildFile(relative),
                                                writable == File() ? File() : writable.getChildFile(relative), root);
        ok &= mergeFile(file, target, backupFolder.getChildFile(relative).getParentDirectory());
    }
    // Remove now-empty folders, deepest first (deleteFile on a folder only removes an empty one).
    Array<File> folders;
    for (const auto& entry : RangedDirectoryIterator(source, true, "*", File::findDirectories))
        folders.add(entry.getFile());
    std::sort(folders.begin(), folders.end(), [](const File& a, const File& b) {
        return a.getFullPathName().length() > b.getFullPathName().length();
    });
    for (const auto& folder : folders) folder.deleteFile();
    if (! source.deleteFile() && source.isDirectory())
        ok &= source.moveFileTo(uniqueDestination(backupFolder.getParentDirectory(), backupFolder.getFileName() + " leftovers"));
    return ok;
}

bool factoryFingerprints(const void* zipData, size_t zipSize, std::map<String, String>& byName) {
    if (zipData == nullptr || zipSize == 0) return false;
    MemoryInputStream stream(zipData, zipSize, false);
    ZipFile zip(stream);
    for (int i = 0; i < zip.getNumEntries(); ++i) {
        std::unique_ptr<InputStream> entry(zip.createStreamForEntry(i));
        if (entry == nullptr) continue;
        Cartridge bank;
        if (bank.load(*entry) == 0) byName[zip.getEntry(i)->filename] = bankFingerprint(bank);
    }
    return ! byName.empty();
}

/** Visible subfolders of `folder`, sorted naturally ignoring case. Reserved
    names ("Profiles", "Cartridges") only matter in the root: deeper down they
    are ordinary user folders. */
Array<File> visibleSubfolders(const File& folder, bool isLibraryRoot) {
    Array<File> found;
    for (const auto& entry : RangedDirectoryIterator(folder, false, "*", File::findDirectories)) {
        const auto name = entry.getFile().getFileName();
        if (isIgnoredName(name) || (isLibraryRoot && isReservedFolderName(name))) continue;
        found.add(entry.getFile());
    }
    std::sort(found.begin(), found.end(), [](const File& a, const File& b) {
        return a.getFileName().compareNatural(b.getFileName(), false) < 0;
    });
    return found;
}

/** Calls `visit` for every visible folder below `top`, parents before their
    children. A symbolic link to a folder already walked (a loop back into the
    library) is not entered again, and the depth is capped, so a link can never
    make the walk endless. */
void forEachFolder(const File& top, bool topIsLibraryRoot, const std::function<void(const File&)>& visit) {
    constexpr int maxDepth = 16;
    std::set<String> walked { top.getFullPathName() };
    std::function<void(const File&, int)> walk = [&](const File& folder, int depth) {
        for (const auto& child : visibleSubfolders(folder, depth == 1 && topIsLibraryRoot)) {
            // Real folders are always shown (as in Finder); only a link whose
            // target was already walked (an ancestor, a loop) is skipped.
            if (child.isSymbolicLink()) {
                if (! walked.insert(child.getLinkedTarget().getFullPathName()).second) continue;
            } else {
                walked.insert(child.getFullPathName());
            }
            visit(child);
            if (depth < maxDepth) walk(child, depth + 1);
        }
    };
    if (top.isDirectory()) walk(top, 1);
}

/** True when `target` exists and is another file or folder than `item`. JUCE
    compares names ignoring case on macOS and Windows, so "pianos" equals
    "Pianos" there; on a disk that ignores case they are the same folder (a
    change of case only), but a case-sensitive volume can hold both, and a
    rename onto the other one would replace it. The file identity decides. */
bool isAnotherItem(const File& target, const File& item) {
    if (! target.exists() || target.getFullPathName() == item.getFullPathName()) return false;
    if (target != item) return true;
    const auto identity = target.getFileIdentifier();
    return identity == 0 || identity != item.getFileIdentifier();
}

/** A folder whose every bank is an untouched factory bank becomes a read-only pack. */
void markFactoryFolder(const File& folder, const std::set<String>& factory, const String& id) {
    if (! folder.isDirectory() || folder.getChildFile(packManifestName).exists()) return;
    int banks = 0;
    for (const auto& entry : RangedDirectoryIterator(folder, true, "*", File::findFiles)) {
        const auto file = entry.getFile();
        if (! isBankFile(file)) continue;
        Cartridge bank;
        if (! readBank(file, bank) || factory.count(bankFingerprint(bank)) == 0) return;
        ++banks;
    }
    if (banks > 0) writePackManifest(folder, id, folder.getFileName());
}
}

//==============================================================================
File rootFor(const File& appDirectory) { return appDirectory; }

uint64 stableHash64(const void* data, size_t size) {
    // 64-bit FNV-1a with the offset used since 0.1.0 (non-standard, but every
    // saved favourite depends on it: do not "fix" it).
    auto* bytes = static_cast<const uint8*>(data);
    uint64 hash = 1469598103934665603ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

String stableHash(const void* data, size_t size) {
    return String::toHexString(static_cast<int64>(stableHash64(data, size)));
}

String bankFingerprint(const Cartridge& bank) {
    return stableHash(bank.getVoiceSysex() + 6, 4096);
}

uint64 voiceHash(const Cartridge& bank, int slot) {
    return stableHash64(bank.getVoiceSysex() + 6 + jlimit(0, 31, slot) * 128, 128);
}

bool isIgnoredName(const String& name) {
    return name.isEmpty() || name.startsWithChar('.') || name.equalsIgnoreCase("__MACOSX")
        || name.endsWithIgnoreCase(".installing") || name.endsWithIgnoreCase(".tmp");
}

String sanitizeFolderName(const String& requested) {
    auto name = requested.removeCharacters("/\\:*?\"<>|");
    String cleaned;
    for (auto c : name)
        if (c >= 32 && c != 127) cleaned << String::charToString(c);
    name = cleaned.trim();
    while (name.startsWithChar('.') || name.startsWithChar(' ')) name = name.substring(1);
    while (name.endsWithChar('.') || name.endsWithChar(' ')) name = name.dropLastCharacters(1);
    name = name.substring(0, 48).trim();
    while (name.endsWithChar('.')) name = name.dropLastCharacters(1).trim();
    static const StringArray devices { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6",
                                       "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
                                       "LPT7", "LPT8", "LPT9" };
    if (devices.contains(name.upToFirstOccurrenceOf(".", false, false).trim(), true)) return {};
    if (isIgnoredName(name) || isReservedFolderName(name)) return {};
    return name;
}

Array<File> listFolders(const File& root, bool nested) {
    if (! root.isDirectory()) return {};
    if (! nested) return visibleSubfolders(root, true);
    // Walked once, cycle-safe: a symbolic link back to the root used to recurse
    // forever here, and nested folders named "Profiles" were hidden.
    Array<File> folders;
    forEachFolder(root, true, [&folders](const File& folder) { folders.add(folder); });
    return folders;
}

String folderOf(const File& file, const File& root) {
    if (! file.isAChildOf(root)) return {};
    const auto parent = file.getParentDirectory();
    if (parent == root) return {};
    return parent.getRelativePathFrom(root).replaceCharacter('\\', '/');
}

File folderNamed(const File& root, const String& name) {
    for (const auto& folder : listFolders(root))
        if (folder.getFileName().equalsIgnoreCase(name)) return folder;
    return root.getChildFile(name);
}

File uniqueDestination(const File& folder, const String& fileName) {
    auto destination = folder.getChildFile(fileName);
    if (! destination.exists()) return destination;
    const auto stem = destination.getFileNameWithoutExtension();
    const auto extension = destination.getFileExtension();
    for (int n = 2; n < 1000; ++n) {
        destination = folder.getChildFile(stem + " " + String(n) + extension);
        if (! destination.exists()) return destination;
    }
    return folder.getNonexistentChildFile(stem, extension, false);
}

bool readBank(const File& file, Cartridge& bank, int* loadResult) {
    if (! file.existsAsFile() || file.getSize() <= 0 || file.getSize() >= 65536) return false;
    const int rc = bank.load(file);
    if (loadResult != nullptr) *loadResult = rc;
    return rc == 0 || rc == 1;
}

bool isPresetFile(const File& file) {
    const auto extension = file.getFileExtension();
    return extension.equalsIgnoreCase(presetExtension) || extension.equalsIgnoreCase(legacyPresetExtension);
}

bool renameLegacyPresets(const File& root) {
    const auto marker = root.getChildFile(".preset-extension-v1");
    if (marker.existsAsFile() || ! root.isDirectory()) return true;
    LibraryScope lock(lockTimeoutMs);
    if (! lock.held) return false;
    // Only the library's own visible folders: hidden ones (backups, removed
    // items) keep their files as they are, and symbolic links are not entered
    // at all: one can lead out of the library, or back into it, endlessly.
    Array<File> legacy;
    std::function<void(const File&, int)> walk = [&](const File& folder, int depth) {
        for (const auto& entry : RangedDirectoryIterator(folder, false, String("*") + legacyPresetExtension, File::findFiles))
            if (! isIgnoredName(entry.getFile().getFileName())) legacy.add(entry.getFile());
        if (depth >= 16) return;
        for (const auto& child : visibleSubfolders(folder, depth == 0))
            if (! child.isSymbolicLink()) walk(child, depth + 1);
    };
    walk(root, 0);
    bool ok = true;
    for (const auto& file : legacy) {
        if (! file.existsAsFile()) continue;
        const auto target = uniqueDestination(file.getParentDirectory(), file.getFileNameWithoutExtension() + presetExtension);
        ok &= ! target.exists() && file.moveFileTo(target);
    }
    if (ok) ok = marker.replaceWithText("completed");
    return ok;
}

File remapPresetPath(const File& file) {
    if (file.exists() || ! file.getFileExtension().equalsIgnoreCase(legacyPresetExtension)) return file;
    const auto renamed = file.withFileExtension(presetExtension);
    return renamed.existsAsFile() ? renamed : file;
}

Cartridge currentBankOf(const File& file, const Cartridge& snapshot) {
    Cartridge bank;
    if (file.getFullPathName().isNotEmpty() && readBank(file, bank)) return bank;
    return snapshot;
}

bool isReadOnly(const File& fileOrFolder, const File& root) {
    for (auto folder = fileOrFolder.isDirectory() ? fileOrFolder : fileOrFolder.getParentDirectory();
         folder.isAChildOf(root); folder = folder.getParentDirectory())
        if (folder.getChildFile(packManifestName).existsAsFile()) return true;
    return false;
}

bool writePackManifest(const File& folder, const String& id, const String& name) {
    auto* object = new DynamicObject();
    object->setProperty("format", "av-dexed-pack");
    object->setProperty("formatVersion", 1);
    object->setProperty("id", id);
    object->setProperty("name", name);
    object->setProperty("readOnly", true);
    const auto text = JSON::toString(var(object));
    return writeAtomically(folder.getChildFile(packManifestName), MemoryBlock(text.toRawUTF8(), text.getNumBytesAsUTF8()));
}

//==============================================================================
InterProcessLock& libraryLock() { static InterProcessLock lock("AVDexedLibrary"); return lock; }
InterProcessLock& preferencesLock() { static InterProcessLock lock("AVDexedPreferences"); return lock; }

LibraryScope::LibraryScope(int timeoutMs) : held(enterLibrary(timeoutMs)) {}
LibraryScope::~LibraryScope() {
    if (! held) return;
    libraryLock().exit();
    libraryMutex().exit();
}

PropertiesFile::Options preferencesOptions() {
    PropertiesFile::Options options;
    options.processLock = &preferencesLock();
    options.millisecondsBeforeSaving = -1;   // saved explicitly (or when it goes out of scope); no timer
    return options;
}

bool writeAtomically(const File& file, const MemoryBlock& data) {
    // Nothing AV Dexed writes is ever empty: an empty block is a failed
    // composition and must not replace the user's file with nothing.
    if (data.isEmpty()) return false;
    TemporaryFile temporary(file, TemporaryFile::useHiddenFile);
    {
        // File::replaceWithData() ignores write errors (a full disk), so a
        // temporary file left empty or cut short would still be swapped over
        // the original. Every step is checked here instead.
        FileOutputStream out(temporary.getFile());
        if (! out.openedOk() || out.getPosition() != 0 || ! out.write(data.getData(), data.getSize()))
            return false;
        out.flush();   // writes what is buffered and syncs it to disk; errors land in the status
        if (! out.getStatus().wasOk())
            return false;
    }
    // The size on disk catches a short write that reported no error.
    return temporary.getFile().getSize() == static_cast<int64>(data.getSize())
        && temporary.overwriteTargetFileWithTemporary();
}

//==============================================================================
BankSettings readEmbeddedSettings(const MemoryBlock& fileBytes) {
    BankSettings settings;
    const auto* bytes = static_cast<const uint8*>(fileBytes.getData());
    const auto size = fileBytes.getSize();
    for (size_t start = 0; start + sizeof(settingsHeader) < size; ++start) {
        if (bytes[start] != 0xf0 || std::memcmp(bytes + start, settingsHeader, sizeof(settingsHeader)) != 0) continue;
        size_t end = start + sizeof(settingsHeader);
        while (end < size && bytes[end] != 0xf7) ++end;
        if (end >= size) break;
        const String json(CharPointer_UTF8(reinterpret_cast<const char*>(bytes + start + sizeof(settingsHeader))),
                          CharPointer_UTF8(reinterpret_cast<const char*>(bytes + end)));
        const auto parsed = JSON::parse(json);
        if (auto* slots = parsed.getProperty("slots", {}).getDynamicObject()) {
            for (const auto& slot : slots->getProperties()) {
                const int index = slot.name.toString().getIntValue();
                if (! isPositiveAndBelow(index, 32)) continue;
                SlotSettings entry;
                entry.voice = static_cast<uint64>(slot.value.getProperty("voice", {}).toString().getHexValue64());
                if (auto* values = slot.value.getProperty("values", {}).getDynamicObject())
                    for (const auto& value : values->getProperties())
                        entry.values[value.name.toString()] = static_cast<float>(static_cast<double>(value.value));
                settings[index] = std::move(entry);
            }
        }
        break;
    }
    return settings;
}

BankSettings readEmbeddedSettings(const File& bank) {
    MemoryBlock bytes;
    if (! bank.existsAsFile() || bank.getSize() >= 65536 || ! bank.loadFileAsData(bytes)) return {};
    return readEmbeddedSettings(bytes);
}

MemoryBlock composeBankFile(const MemoryBlock& originalBytes, Cartridge bank, const BankSettings& settings) {
    MemoryBlock result;
    const auto* bytes = static_cast<const uint8*>(originalBytes.getData());
    const auto size = originalBytes.getSize();
    const auto* voice = reinterpret_cast<const uint8*>(bank.getVoiceSysex());   // non-const: refreshes header + checksum
    bool bankWritten = false;
    for (size_t i = 0; i < size;) {
        if (bytes[i] != 0xf0) { result.append(bytes + i, 1); ++i; continue; }
        size_t end = i + 1;
        while (end < size && bytes[end] != 0xf7) ++end;
        if (end >= size) { result.append(bytes + i, size - i); break; }   // unterminated tail: keep as is
        const auto length = end - i + 1;
        const bool isBank = ! bankWritten && length == SYSEX_SIZE && bytes[i + 1] == 0x43 && (bytes[i + 2] & 0xf0) == 0
                            && bytes[i + 3] == 9 && bytes[i + 4] == 0x20 && bytes[i + 5] == 0;
        const bool isSettings = length > sizeof(settingsHeader)
                                && std::memcmp(bytes + i, settingsHeader, sizeof(settingsHeader)) == 0;
        if (isBank) { result.append(voice, SYSEX_SIZE); bankWritten = true; }
        else if (! isSettings) result.append(bytes + i, length);
        i = end + 1;
    }
    if (! bankWritten) {
        MemoryBlock withBank(voice, SYSEX_SIZE);
        withBank.append(result.getData(), result.getSize());
        result = withBank;
    }
    if (! settings.empty()) {
        auto* slots = new DynamicObject();
        for (const auto& [index, slot] : settings) {
            auto* entry = new DynamicObject();
            entry->setProperty("voice", String::toHexString(static_cast<int64>(slot.voice)));
            auto* values = new DynamicObject();
            for (const auto& [id, value] : slot.values) values->setProperty(id, value);
            entry->setProperty("values", var(values));
            slots->setProperty(String(index), var(entry));
        }
        auto* root = new DynamicObject();
        root->setProperty("format", "av-dexed-sounds");
        root->setProperty("formatVersion", 1);
        root->setProperty("slots", var(slots));
        const auto json = JSON::toString(var(root), JSON::FormatOptions{}.withSpacing(JSON::Spacing::none)
                                                        .withEncoding(JSON::Encoding::ascii).withMaxDecimalPlaces(6));
        result.append(settingsHeader, sizeof(settingsHeader));
        result.append(json.toRawUTF8(), json.getNumBytesAsUTF8());   // ASCII only: 7-bit safe inside SysEx
        const uint8 endOfExclusive = 0xf7;
        result.append(&endOfExclusive, 1);
    }
    return result;
}

//==============================================================================
namespace {
uint64 contentHash(const MemoryBlock& bytes) {
    return stableHash64(bytes.getData(), bytes.getSize()) ^ static_cast<uint64>(bytes.getSize());
}

/** Writes `bytes` into `folder` as `fileName` unless the folder already holds
    the same bytes (under any name). Never overwrites: a different file with the
    name gets " 2". */
ImportResult writeIntoFolder(const MemoryBlock& bytes, const String& extension, const File& folder,
                             FolderIndex* index, const String& fileName) {
    ImportResult result;
    const auto hash = contentHash(bytes);
    LibraryScope lock(lockTimeoutMs);
    if (! lock.held || ! folder.createDirectory().wasOk()) return result;
    FolderIndex local;
    auto& known = index != nullptr ? *index : local;
    if (! known.built || known.folder != folder) {
        known.folder = folder;
        known.files.clear();
        for (const auto& entry : RangedDirectoryIterator(folder, false, "*", File::findFiles)) {
            const auto existing = entry.getFile();
            MemoryBlock existingBytes;
            if (isSoundFile(existing) && existing.getSize() <= 32 * 1024 * 1024 && existing.loadFileAsData(existingBytes))
                known.files.emplace(contentHash(existingBytes), existing);
        }
        known.built = true;
    }
    const auto candidates = known.files.equal_range(hash);
    for (auto candidate = candidates.first; candidate != candidates.second; ++candidate) {
        MemoryBlock existingBytes;
        if ((candidate->second.getFileExtension().equalsIgnoreCase(extension)
             || (isPresetFile(candidate->second) && isPresetFile(File("x" + extension))))
            && candidate->second.loadFileAsData(existingBytes) && existingBytes == bytes) {
            result.status = ImportResult::Status::alreadyThere;
            result.file = candidate->second;
            return result;
        }
    }
    const auto destination = uniqueDestination(folder, fileName);
    if (writeAtomically(destination, bytes)) {
        result.status = ImportResult::Status::copied;
        result.file = destination;
        known.files.emplace(hash, destination);
    }
    return result;
}

/** Copies `source` into `folder` unless the folder already holds the same bytes. */
ImportResult copyIntoFolder(const File& source, const File& folder, FolderIndex* index, const String& fileName) {
    MemoryBlock bytes;
    if (! source.loadFileAsData(bytes)) return {};
    return writeIntoFolder(bytes, source.getFileExtension(), folder, index, fileName);
}

/** One standard bank, complete messages only, under 64 KB: the files the
    library keeps as they are (and "Guardar" can rewrite). */
bool isPlainBankFile(const MemoryBlock& bytes) {
    const auto* data = static_cast<const uint8*>(bytes.getData());
    const auto size = bytes.getSize();
    if (size == 0 || size >= 65536 || data[0] != 0xF0) return false;
    int banks = 0;
    size_t pos = 0;
    while (pos < size) {
        if (data[pos] != 0xF0) return false;
        size_t end = pos + 1;
        while (end < size && data[end] != 0xF7) ++end;
        if (end >= size) return false;
        if (end - pos + 1 == size_t(SYSEX_SIZE) && data[pos + 1] == 0x43 && (data[pos + 2] & 0xF0) == 0
            && data[pos + 3] == 9 && data[pos + 4] == 0x20 && data[pos + 5] == 0)
            ++banks;
        pos = end + 1;
    }
    return banks == 1;
}

bool readImportable(const File& source, MemoryBlock& bytes) {
    return source.existsAsFile() && source.getSize() > 0 && source.getSize() <= maxImportBytes
        && source.loadFileAsData(bytes);
}

/** A standard bank file for a bank found in a dump: its AMEM first (the order
    of a DX7II/TX802 bank dump), then the 32 voices. */
MemoryBlock standardBankFile(const AVDexedSysex::Bank& bank) {
    MemoryBlock file;
    if (bank.hasAmem) {
        const auto amem = AVDexedSysex::amemMessage(bank.amem.data());
        file.append(amem.data(), amem.size());
    }
    const auto voices = AVDexedSysex::bankMessage(bank.voices.data());
    file.append(voices.data(), voices.size());
    return file;
}

/** Single voices as banks of 32: the voices in order, the rest Init. */
std::vector<std::pair<MemoryBlock, int>> voicesToBankFiles(const std::vector<std::array<uint8_t, AVDexedSysex::voiceBytes>>& voices) {
    std::vector<std::pair<MemoryBlock, int>> files;
    const auto init = Program::initialProgram();
    for (size_t first = 0; first < voices.size(); first += 32) {
        Cartridge bank;
        for (int slot = 0; slot < 32; ++slot)
            bank.packProgram(init.getRawData(), slot, "INIT VOICE");
        const int count = int(std::min<size_t>(32, voices.size() - first));
        for (int slot = 0; slot < count; ++slot) {
            auto packed = AVDexedSysex::packVoice(voices[first + size_t(slot)].data());
            bank.setPackedProgram(slot, reinterpret_cast<char*>(packed.data()));
        }
        const auto message = AVDexedSysex::bankMessage(reinterpret_cast<const uint8_t*>(bank.getRawVoice()));
        files.emplace_back(MemoryBlock(message.data(), message.size()), count);
    }
    return files;
}

/** "name" for one bank, "name A"/"name B" for two, "name 01"… for more. */
String bankFileName(const String& stem, size_t index, size_t count) {
    if (count <= 1) return stem;
    if (count == 2) return stem + (index == 0 ? " A" : " B");
    return stem + " " + String(int(index + 1)).paddedLeft('0', count > 99 ? 3 : 2);
}

/** Writes every produced bank; copied when any was written, alreadyThere when
    the folder held all of them. */
ImportResult writeBankFiles(const std::vector<std::pair<MemoryBlock, int>>& banks, const String& stem,
                            const File& folder, FolderIndex* index, bool numberFromOne) {
    ImportResult result;
    int copied = 0, reused = 0;
    for (size_t i = 0; i < banks.size(); ++i) {
        const auto name = numberFromOne && banks.size() > 1 ? stem + " " + String(int(i + 1)) : bankFileName(stem, i, banks.size());
        const auto written = writeIntoFolder(banks[i].first, ".syx", folder, index, name + ".syx");
        if (written.file == File()) continue;
        result.files.add(written.file);
        if (written.status == ImportResult::Status::copied) { ++copied; result.sounds += banks[i].second; }
        else ++reused;
    }
    result.file = result.files.isEmpty() ? File() : result.files.getFirst();
    result.status = copied > 0 ? ImportResult::Status::copied
                  : reused > 0 && reused == int(banks.size()) ? ImportResult::Status::alreadyThere
                  : ImportResult::Status::failed;
    return result;
}
}

ImportResult importBank(const File& source, const File& folder, FolderIndex* index) {
    ImportResult result;
    MemoryBlock bytes;
    if (! readImportable(source, bytes)) {
        result.status = ImportResult::Status::notABank;
        return result;
    }
    Cartridge bank;
    if (isPlainBankFile(bytes) && readBank(source, bank)) {
        if (source.getParentDirectory() == folder) {
            result.status = ImportResult::Status::alreadyThere;
            result.file = source;
            result.files.add(source);
            return result;
        }
        result = copyIntoFolder(source, folder, index, source.getFileName());
        if (result.file != File()) result.files.add(result.file);
        if (result.status == ImportResult::Status::copied) result.sounds = 32;
        return result;
    }
    const auto contents = AVDexedSysex::scan(bytes);
    std::vector<std::pair<MemoryBlock, int>> banks;
    std::vector<const AVDexedSysex::Bank*> distinct;   // a bank repeated in one file is one bank
    for (const auto& found : contents.banks)
        if (std::none_of(distinct.begin(), distinct.end(), [&found](const auto* kept) { return kept->voices == found.voices; }))
            distinct.push_back(&found);
    for (const auto* found : distinct)
        banks.emplace_back(standardBankFile(*found), 32);
    if (banks.empty())
        banks = voicesToBankFiles(contents.voices);
    if (banks.empty()) {
        result.status = ImportResult::Status::notABank;
        return result;
    }
    return writeBankFiles(banks, source.getFileNameWithoutExtension(), folder, index, false);
}

bool isSingleVoiceFile(const File& file) {
    MemoryBlock bytes;
    if (! file.getFileExtension().equalsIgnoreCase(".syx") || file.getSize() > 64 * 1024 || ! readImportable(file, bytes))
        return false;
    const auto contents = AVDexedSysex::scan(bytes);
    return contents.banks.empty() && ! contents.voices.empty();
}

ImportResult importVoices(const Array<File>& sources, const File& folder, const String& bankName, FolderIndex* index) {
    auto ordered = sources;
    std::sort(ordered.begin(), ordered.end(), [](const File& a, const File& b) {
        return a.getFileName().compareNatural(b.getFileName()) < 0;
    });
    std::vector<std::array<uint8_t, AVDexedSysex::voiceBytes>> voices;
    for (const auto& source : ordered) {
        MemoryBlock bytes;
        if (! readImportable(source, bytes)) continue;
        const auto contents = AVDexedSysex::scan(bytes);
        voices.insert(voices.end(), contents.voices.begin(), contents.voices.end());
    }
    if (voices.empty()) {
        ImportResult result;
        result.status = ImportResult::Status::notABank;
        return result;
    }
    const auto name = sanitizeFolderName(bankName).isNotEmpty() ? sanitizeFolderName(bankName) : String("Voices");
    return writeBankFiles(voicesToBankFiles(voices), name, folder, index, true);
}

ImportResult importPreset(const File& source, const File& folder, FolderIndex* index) {
    ImportResult result;
    if (! source.existsAsFile() || source.getSize() <= 0 || source.getSize() > 32 * 1024 * 1024) {
        result.status = ImportResult::Status::notABank;
        return result;
    }
    if (source.getParentDirectory() == folder) {
        result.status = ImportResult::Status::alreadyThere;
        result.file = source;
        return result;
    }
    // An old .dexedpreset arrives under the current name; the format is the same.
    return copyIntoFolder(source, folder, index, source.getFileNameWithoutExtension() + presetExtension);
}

//==============================================================================
StringArray libraryFingerprints(const File& root) {
    StringArray fingerprints;
    for (const auto& entry : RangedDirectoryIterator(root, true, "*", File::findFiles | File::ignoreHiddenFiles)) {
        const auto file = entry.getFile();
        if (! isBankFile(file) || file.getRelativePathFrom(root).startsWithChar('.')) continue;
        Cartridge bank;
        if (readBank(file, bank)) fingerprints.addIfNotAlreadyThere(bankFingerprint(bank));
    }
    return fingerprints;
}

int installFactorySounds(const File& root, const void* zipData, size_t zipSize, bool restoreMissing) {
    if (zipData == nullptr || zipSize == 0) return 0;
    LibraryScope lock(lockTimeoutMs);
    if (! lock.held) return 0;
    const auto present = restoreMissing ? libraryFingerprints(root) : StringArray();
    MemoryInputStream stream(zipData, zipSize, false);
    ZipFile zip(stream);
    int written = 0;
    for (int i = 0; i < zip.getNumEntries(); ++i) {
        const auto name = zip.getEntry(i)->filename;
        if (! name.endsWithIgnoreCase(".syx")) continue;
        MemoryBlock bytes;
        {
            std::unique_ptr<InputStream> entry(zip.createStreamForEntry(i));
            if (entry == nullptr) continue;
            entry->readIntoMemoryBlock(bytes);
        }
        Cartridge bank;
        if (bank.load(static_cast<const uint8_t*>(bytes.getData()), static_cast<int>(bytes.getSize())) != 0) continue;
        if (restoreMissing && present.contains(bankFingerprint(bank))) continue;
        const bool isDexed = name.equalsIgnoreCase("Dexed_01.syx");
        const auto folder = folderNamed(root, isDexed ? factoryFolderName : synprezFolderName);
        const bool created = ! folder.exists();
        if (! folder.createDirectory().wasOk()) continue;
        if (created) writePackManifest(folder, isDexed ? "av-dexed-factory" : "av-dexed-synprezfm", folder.getFileName());
        auto destination = folder.getChildFile(name);
        if (destination.exists()) {
            if (! restoreMissing) continue;
            destination = uniqueDestination(folder, name);
        }
        if (writeAtomically(destination, bytes)) ++written;
    }
    return written;
}

bool migratePreferences(const File& appDirectory, const String& newName) {
    const auto oldFile = appDirectory.getChildFile("Dexed.xml");
    if (! oldFile.existsAsFile()) return true;
    ScopedLibraryLock lock(preferencesLock());
    if (! lock.held) return false;
    if (! oldFile.existsAsFile()) return true;   // another instance did it meanwhile
    {
        PropertiesFile::Options readOnly;
        readOnly.millisecondsBeforeSaving = -1;
        PropertiesFile previous(oldFile, readOnly);
        PropertiesFile current(appDirectory.getChildFile(newName), preferencesOptions());
        const auto& keys = previous.getAllProperties().getAllKeys();
        for (const auto& key : keys) {
            const auto value = previous.getValue(key);
            if (key == "favoritePresetIds") {
                // Favourites are joined: none is ever lost to a rename or a rollback.
                auto joined = StringArray::fromLines(current.getValue(key));
                for (const auto& favourite : StringArray::fromLines(value))
                    if (favourite.isNotEmpty()) joined.addIfNotAlreadyThere(favourite);
                joined.removeEmptyStrings();
                current.setValue(key, joined.joinIntoString("\n"));
            } else if (! current.containsKey(key)) {
                current.setValue(key, value);
            }
        }
        if (! current.saveIfNeeded()) return false;
    }
    const auto backup = appDirectory.getChildFile(backupFolderName);
    if (! backup.createDirectory().wasOk()) return false;
    const auto stamp = Time::getCurrentTime().formatted("%Y-%m-%d %H%M%S");
    return oldFile.moveFileTo(uniqueDestination(backup, "Dexed " + stamp + ".xml"));
}

bool migrateLayout(const File& appDirectory, const void* factoryZip, size_t factoryZipSize) {
    LibraryScope lock(lockTimeoutMs);
    if (! lock.held) return false;   // another AV Dexed is migrating; this launch skips it
    const auto backup = appDirectory.getChildFile(backupFolderName);
    const auto marker = appDirectory.getChildFile(".library-layout-v2");
    bool ok = true;

    const auto cartridges = appDirectory.getChildFile("Cartridges");
    const bool upgrading = cartridges.isDirectory();
    if (upgrading) {
        const auto cartridgeBackup = backup.getChildFile("Cartridges");
        // A pack's writable twin ("SynprezFM 2"); loose in the root if no name is left.
        const auto writableTwin = [&appDirectory](const String& folderName) {
            const auto twin = writableFolderFor(appDirectory, folderName);
            return twin == File() ? appDirectory : twin;
        };
        Array<File> entries;
        for (const auto& entry : RangedDirectoryIterator(cartridges, false, "*", File::findFilesAndDirectories))
            entries.add(entry.getFile());
        for (const auto& entry : entries) {
            const auto name = entry.getFileName();
            if (entry.isDirectory()) {
                auto folderName = sanitizeFolderName(name);
                if (folderName.isEmpty()) folderName = sanitizeFolderName(name + " (Cartridges)");
                if (folderName.isEmpty()) folderName = "Cartridges folder";
                const auto destination = folderNamed(appDirectory, folderName);
                // Never new or changed files into a read-only pack ("SynprezFM"):
                // those go to its writable twin.
                const auto writable = isReadOnly(destination, appDirectory) ? writableTwin(folderName) : File();
                ok &= destination.exists() ? mergeFolder(entry, destination, cartridgeBackup.getChildFile(name), writable, appDirectory)
                                           : entry.moveFileTo(destination);
            } else if (isBankFile(entry)) {
                // Dexed's own startup bank goes with the factory sounds (unless it
                // differs from the pack's copy: then it is the user's, and goes to
                // a writable "Dexed Factory 2"); any other loose bank stays loose
                // ("Sin carpeta").
                const bool startupBank = name.equalsIgnoreCase("Dexed_01.syx");
                const auto folder = startupBank ? folderNamed(appDirectory, factoryFolderName) : appDirectory;
                const auto writable = startupBank && isReadOnly(folder, appDirectory) ? writableTwin(factoryFolderName) : File();
                ok &= mergeFile(entry, packSafeDestination(entry, folder.getChildFile(name),
                                                           writable == File() ? File() : writable.getChildFile(name), appDirectory),
                                cartridgeBackup);
            } else {
                ok &= archive(entry, cartridgeBackup);
            }
        }
        if (! cartridges.deleteFile() && cartridges.isDirectory())
            ok &= cartridges.moveFileTo(uniqueDestination(backup, "Cartridges"));
    }

    const auto profiles = appDirectory.getChildFile("Profiles");
    if (profiles.isDirectory()) {
        const auto hidden = appDirectory.getChildFile(profilesFolderName);
        ok &= hidden.exists() ? mergeFolder(profiles, hidden, backup.getChildFile("Profiles")) : profiles.moveFileTo(hidden);
    }

    if (! marker.existsAsFile()) {
        bool anyBank = false;
        for (const auto& folder : listFolders(appDirectory, false)) {
            for (const auto& entry : RangedDirectoryIterator(folder, true, "*.syx", File::findFiles))
                if (isBankFile(entry.getFile())) { anyBank = true; break; }
            if (anyBank) break;
        }
        if (! upgrading && ! anyBank)
            ok &= installFactorySounds(appDirectory, factoryZip, factoryZipSize, false) > 0;
        std::map<String, String> factoryByName;
        if (factoryFingerprints(factoryZip, factoryZipSize, factoryByName)) {
            std::set<String> factory;
            for (const auto& [name, fingerprint] : factoryByName) factory.insert(fingerprint);
            markFactoryFolder(folderNamed(appDirectory, factoryFolderName), factory, "av-dexed-factory");
            markFactoryFolder(folderNamed(appDirectory, synprezFolderName), factory, "av-dexed-synprezfm");
        }
        if (ok) ok &= marker.replaceWithText("completed");
    }
    // 0.2.0: sound files are called .avdexpreset (the format is unchanged).
    ok &= renameLegacyPresets(appDirectory);
    return ok;
}

File remapLegacyPath(const File& file, const File& appDirectory) {
    if (file.exists() || file == File()) return file;
    const auto cartridges = appDirectory.getChildFile("Cartridges");
    if (! file.isAChildOf(cartridges)) return file;
    const auto relative = file.getRelativePathFrom(cartridges).replaceCharacter('\\', '/');
    File candidate;
    if (relative.equalsIgnoreCase("Dexed_01.syx"))
        candidate = folderNamed(appDirectory, factoryFolderName).getChildFile(file.getFileName());
    else if (relative.containsChar('/'))
        candidate = appDirectory.getChildFile(relative);
    else
        candidate = appDirectory.getChildFile(file.getFileName());
    return candidate.existsAsFile() ? candidate : file;
}

ExternalAdoption copyExternalSounds(const File& appDirectory, const File& preferencesFile) {
    ExternalAdoption adoption;
    StringArray banks, presets;
    {
        // Read only: nothing is written here, so no lock is needed.
        PropertiesFile::Options readOnly;
        readOnly.millisecondsBeforeSaving = -1;
        PropertiesFile preferences(preferencesFile, readOnly);
        banks = StringArray::fromLines(preferences.getValue("presetBrowserKnownBanks"));
        presets = StringArray::fromLines(preferences.getValue("presetBrowserKnownPresets"));
    }
    banks.removeEmptyStrings();
    presets.removeEmptyStrings();
    if (banks.isEmpty() && presets.isEmpty()) return adoption;
    const auto known = libraryFingerprints(appDirectory);
    for (const auto& path : banks) {
        const File file(path);
        // Inside the data folder: part of the library now. Missing (e.g. a drive
        // that is not mounted): stays pending and is retried on a later launch.
        if (file.isAChildOf(appDirectory)) { adoption.settledBanks.add(path); continue; }
        if (! file.existsAsFile()) continue;
        Cartridge bank;
        if (! readBank(file, bank)) { adoption.settledBanks.add(path); continue; }   // not a bank (any more)
        if (known.contains(bankFingerprint(bank))) { adoption.settledBanks.add(path); continue; }
        auto name = sanitizeFolderName(file.getFileNameWithoutExtension());
        if (name.isEmpty()) name = "Bancos";
        // Never into a read-only pack that happens to have the bank's name.
        auto folder = writableFolderFor(appDirectory, name);
        if (folder == File()) folder = folderNamed(appDirectory, "Bancos");
        const auto result = importBank(file, folder);
        if (result.status == ImportResult::Status::copied || result.status == ImportResult::Status::alreadyThere) {
            adoption.copied.add(file.getFileNameWithoutExtension());
            adoption.settledBanks.add(path);
        }
    }
    for (const auto& path : presets) {
        const File file(path);
        if (file.isAChildOf(appDirectory)) { adoption.settledPresets.add(path); continue; }
        if (! file.existsAsFile()) continue;
        const auto result = importPreset(file, folderNamed(appDirectory, userPresetsFolderName));
        if (result.status == ImportResult::Status::copied || result.status == ImportResult::Status::alreadyThere) {
            adoption.copied.add(file.getFileNameWithoutExtension());
            adoption.settledPresets.add(path);
        } else if (result.status == ImportResult::Status::notABank) {
            adoption.settledPresets.add(path);
        }
    }
    return adoption;
}

void settleExternalSounds(const File& preferencesFile, const ExternalAdoption& adoption) {
    if (adoption.settledBanks.isEmpty() && adoption.settledPresets.isEmpty()) return;
    ScopedLibraryLock prefsLock(preferencesLock());
    // Re-read right before writing: only the settled paths leave the lists, so
    // a path another instance added meanwhile is kept, and so is every other key.
    PropertiesFile preferences(preferencesFile, preferencesOptions());
    const auto settle = [&preferences](const char* key, const StringArray& settled) {
        auto pending = StringArray::fromLines(preferences.getValue(key));
        pending.removeEmptyStrings();
        StringArray kept;
        for (const auto& path : pending)
            if (! settled.contains(path)) kept.add(path);
        if (kept.size() != pending.size()) preferences.setValue(key, kept.joinIntoString("\n"));
    };
    settle("presetBrowserKnownBanks", adoption.settledBanks);
    settle("presetBrowserKnownPresets", adoption.settledPresets);
    preferences.saveIfNeeded();
}

StringArray adoptExternalSounds(const File& appDirectory, const File& preferencesFile) {
    const auto adoption = copyExternalSounds(appDirectory, preferencesFile);
    settleExternalSounds(preferencesFile, adoption);
    return adoption.copied;
}

//==============================================================================
bool isSoundFile(const File& file) {
    const auto extension = file.getFileExtension();
    return (extension.equalsIgnoreCase(".syx") || isPresetFile(file))
        && ! isIgnoredName(file.getFileName());
}

Array<File> findSoundFiles(const File& root) {
    Array<File> files;
    const auto addSoundsOf = [&files](const File& folder) {
        Array<File> level;
        for (const auto& entry : RangedDirectoryIterator(folder, false, "*", File::findFiles))
            if (isSoundFile(entry.getFile())) level.add(entry.getFile());
        std::sort(level.begin(), level.end(), [](const File& a, const File& b) {
            return a.getFileName().compareNatural(b.getFileName(), false) < 0;
        });
        files.addArray(level);
    };
    if (! root.isDirectory()) return files;
    addSoundsOf(root);
    forEachFolder(root, true, addSoundsOf);
    return files;
}

String librarySignature(const File& root) {
    if (! root.isDirectory()) return {};
    StringArray lines;
    // File times come from File (milliseconds): the directory iterator's own
    // times are whole seconds on macOS, too coarse for a save right after a load.
    for (const auto& entry : RangedDirectoryIterator(root, false, "*", File::findFiles))
        if (isSoundFile(entry.getFile()))
            lines.add(entry.getFile().getFileName() + "|" + String(entry.getFileSize()) + "|"
                      + String(entry.getFile().getLastModificationTime().toMilliseconds()));
    forEachFolder(root, true, [&lines, &root](const File& folder) {
        // A folder's time changes when an entry is added, removed or renamed in
        // it, including the atomic rename that every library write ends with.
        lines.add(folder.getRelativePathFrom(root) + "/|" + String(folder.getLastModificationTime().toMilliseconds()));
    });
    lines.sort(false);
    const auto text = lines.joinIntoString("\n");
    return stableHash(text.toRawUTF8(), text.getNumBytesAsUTF8());
}

FolderContents describeFolder(const File& folder) {
    FolderContents contents;
    if (! folder.isDirectory()) return contents;
    for (const auto& entry : RangedDirectoryIterator(folder, true, "*", File::findFiles, File::FollowSymlinks::noCycles)) {
        const auto file = entry.getFile();
        if (isLeftover(file.getFileName())) continue;
        // Sounds under a hidden name (".old/x.syx") go with the folder but are
        // not shown anywhere: they count as other files.
        bool visible = isSoundFile(file);
        for (auto parent = file.getParentDirectory(); visible && parent.isAChildOf(folder); parent = parent.getParentDirectory())
            visible = ! isIgnoredName(parent.getFileName());
        Cartridge bank;
        if (visible && isPresetFile(file)) ++contents.presets;
        else if (visible && readBank(file, bank)) ++contents.banks;
        else ++contents.otherFiles;
    }
    return contents;
}

RenameResult renameFolder(const File& folder, const String& requestedName, const File& root, File* renamedTo) {
    if (! folder.isDirectory() || ! folder.isAChildOf(root)) return RenameResult::notFound;
    if (isReadOnly(folder, root)) return RenameResult::readOnly;
    const auto name = sanitizeFolderName(requestedName);
    if (name.isEmpty()) return RenameResult::invalidName;
    LibraryScope lock(lockTimeoutMs);
    if (! lock.held || ! folder.isDirectory()) return RenameResult::failed;
    const auto target = folder.getSiblingFile(name);
    if (target.getFullPathName() != folder.getFullPathName()) {
        // A change of case only is the same folder and is allowed. JUCE's
        // moveFileTo deletes an existing target first (and a rename replaces
        // one), so it is never called onto another item.
        if (isAnotherItem(target, folder)) return RenameResult::alreadyExists;
        if (! folder.moveFileTo(target) || ! target.isDirectory()) return RenameResult::failed;
    }
    if (renamedTo != nullptr) *renamedTo = target;
    return RenameResult::renamed;
}

RenameResult renameSoundFile(const File& file, const String& requestedStem, const File& root, File* renamedTo) {
    if (! file.existsAsFile() || ! file.isAChildOf(root)) return RenameResult::notFound;
    if (isReadOnly(file, root)) return RenameResult::readOnly;
    const auto stem = sanitizeFolderName(requestedStem);   // the same rules make a portable file name
    if (stem.isEmpty()) return RenameResult::invalidName;
    LibraryScope lock(lockTimeoutMs);
    if (! lock.held || ! file.existsAsFile()) return RenameResult::failed;
    auto target = file.getSiblingFile(stem + file.getFileExtension());
    if (target.getFullPathName() != file.getFullPathName()) {
        if (isAnotherItem(target, file))
            target = uniqueDestination(file.getParentDirectory(), target.getFileName());
        if (isAnotherItem(target, file)) return RenameResult::failed;
        if (! file.moveFileTo(target) || ! target.existsAsFile()) return RenameResult::failed;
    }
    if (renamedTo != nullptr) *renamedTo = target;
    return RenameResult::renamed;
}

#if JUCE_WINDOWS
namespace {
/** Follows IFileOperation's delete of one item. Windows deletes for good what
    it cannot recycle (a drive or share without a Recycle Bin, "Don't move files
    to the Recycle Bin", an item too big for it), without asking when no
    confirmation is wanted: such an item is refused before it is touched, and
    the item only counts as recycled once it is really in the Recycle Bin. */
class RecycleOnlySink final : public IFileOperationProgressSink {
public:
    bool recycled() const noexcept { return reachedRecycleBin && ! destroyed; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (object == nullptr) return E_POINTER;
        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_IFileOperationProgressSink)) {
            *object = static_cast<IFileOperationProgressSink*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    // It lives on the stack for one operation: the count is kept, never used to delete.
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }

    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem*) override {
        // Without this flag Windows is about to delete the item permanently.
        return (flags & recycleIfPossible) != 0 ? S_OK : E_ABORT;
    }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT result, IShellItem* inRecycleBin) override {
        // `inRecycleBin` is the item in the Recycle Bin, or null when it was deleted for good.
        if (SUCCEEDED(result)) {
            if (inRecycleBin != nullptr) reachedRecycleBin = true;
            else destroyed = true;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

private:
    static constexpr DWORD recycleIfPossible = 0x80;   // TSF_DELETE_RECYCLE_IF_POSSIBLE
    std::atomic<ULONG> references { 1 };
    bool reachedRecycleBin = false, destroyed = false;
};

/** Hands `item` to the Recycle Bin, never deleting it for good. False when the
    Recycle Bin would not keep it (the item then stays where it is). */
bool moveToRecycleBin(const File& item) {
    const auto path = item.getFullPathName();
    // A drive or share without a Recycle Bin (a network folder, a redirected
    // profile) has nowhere to keep it.
    SHQUERYRBINFO recycleBin {};
    recycleBin.cbSize = sizeof(recycleBin);
    if (FAILED(SHQueryRecycleBinW(path.toWideCharPointer(), &recycleBin))) return false;

    // IFileOperation needs a single-threaded apartment (the message thread's).
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return false;
    bool recycled = false;
    {
        RecycleOnlySink sink;   // outlives the operation, which holds it while it runs
        IFileOperation* operation = nullptr;
        IShellItem* shellItem = nullptr;
        BOOL aborted = FALSE;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_IFileOperation,
                                       reinterpret_cast<void**>(&operation)))
            && SUCCEEDED(operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT))
            && SUCCEEDED(SHCreateItemFromParsingName(path.toWideCharPointer(), nullptr, IID_IShellItem,
                                                     reinterpret_cast<void**>(&shellItem)))
            && SUCCEEDED(operation->DeleteItem(shellItem, &sink))
            && SUCCEEDED(operation->PerformOperations())
            && SUCCEEDED(operation->GetAnyOperationsAborted(&aborted)))
            recycled = ! aborted && sink.recycled();
        if (shellItem != nullptr) shellItem->Release();
        if (operation != nullptr) operation->Release();
    }
    CoUninitialize();
    return recycled;
}
}
#endif

RemoveResult removeToTrash(const File& item, const File& root, bool useSystemTrash) {
    RemoveResult result;
    const auto hidden = root.getChildFile(deletedFolderName);
    if (! item.exists() || ! item.isAChildOf(root) || item == hidden || item.isAChildOf(hidden)) return result;
    if (isReadOnly(item, root)) {
        result.status = RemoveResult::Status::readOnly;
        return result;
    }
    {
        LibraryScope lock(lockTimeoutMs);
        if (! lock.held || ! item.exists() || ! hidden.createDirectory().wasOk()) return result;
        const auto stamp = Time::getCurrentTime().formatted("%Y-%m-%d %H%M%S");
        const auto name = item.isDirectory() ? item.getFileName() + " " + stamp
                                             : item.getFileNameWithoutExtension() + " " + stamp + item.getFileExtension();
        const auto destination = uniqueDestination(hidden, name);
        if (destination.exists() || ! item.moveFileTo(destination) || ! destination.exists()) return result;
        result.hiddenCopy = destination;
    }
    // The rename above already took it out of the library; the trash is the
    // user's way back. Where the system has no trash (or refuses it), it stays
    // in `.deleted`: still recoverable, never destroyed by AV Dexed.
   #if JUCE_WINDOWS
    // JUCE's moveToTrash() lets Windows delete for good what it cannot recycle.
    const bool handedToTrash = useSystemTrash && moveToRecycleBin(result.hiddenCopy);
   #else
    const bool handedToTrash = useSystemTrash && result.hiddenCopy.moveToTrash();
   #endif
    const bool trashed = handedToTrash && ! result.hiddenCopy.exists();
    result.status = trashed ? RemoveResult::Status::trashed : RemoveResult::Status::keptHidden;
    return result;
}

std::vector<LegacyFolder> parseLegacyFolders(const String& xml) {
    std::vector<LegacyFolder> folders;
    const auto document = XmlDocument::parse(xml);
    if (document == nullptr || ! document->hasTagName("PresetFolders")) return folders;
    for (const auto* element : document->getChildWithTagNameIterator("Folder")) {
        LegacyFolder folder;
        folder.id = element->getStringAttribute("id");
        folder.name = element->getStringAttribute("name").trim();
        for (const auto* key : element->getChildWithTagNameIterator("PresetKey"))
            if (key->getStringAttribute("value").isNotEmpty())
                folder.keys.addIfNotAlreadyThere(key->getStringAttribute("value"));
        if (folder.id.isNotEmpty() && folder.name.isNotEmpty())
            folders.push_back(std::move(folder));
    }
    return folders;
}

String composeLegacyFolders(const std::vector<LegacyFolder>& folders) {
    if (folders.empty()) return {};
    XmlElement catalog("PresetFolders");
    for (const auto& folder : folders) {
        auto* element = catalog.createNewChildElement("Folder");
        element->setAttribute("id", folder.id);
        element->setAttribute("name", folder.name);
        for (const auto& key : folder.keys)
            element->createNewChildElement("PresetKey")->setAttribute("value", key);
    }
    return catalog.toString();
}

File writableFolderFor(const File& root, const String& requestedName) {
    const auto name = sanitizeFolderName(requestedName);
    if (name.isEmpty()) return {};
    const auto usable = [&root](const File& folder) {
        return ! folder.exists() || (folder.isDirectory() && ! isReadOnly(folder, root));
    };
    auto folder = folderNamed(root, name);
    const auto base = folder.exists() ? folder.getFileName() : name;   // "SynprezFM 2", not "synprezfm 2"
    for (int n = 2; ! usable(folder); ++n) {
        if (n >= 1000) return {};
        const auto numbered = sanitizeFolderName(base.substring(0, 42).trim() + " " + String(n));
        if (numbered.isEmpty()) return {};
        folder = folderNamed(root, numbered);
    }
    return folder;
}

}
