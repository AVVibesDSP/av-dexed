// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedLibrary.h"
#include "AVDexedSysex.h"
#include "Program.h"
#include <array>
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

#if ! JUCE_WINDOWS
 #include <dirent.h>
 #include <sys/stat.h>
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
    return name.equalsIgnoreCase("Cartridges") || name.equalsIgnoreCase("Profiles")
        || name.equalsIgnoreCase(syxBackupsFolderName);
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
    return moveFile(file, uniqueDestination(backupFolder, file.getFileName()));
}

/** Moves `source` to `destination` without ever replacing anything: identical
    content already there → the source is archived; different → " 2". */
bool mergeFile(const File& source, const File& destination, const File& backupFolder) {
    if (! destination.getParentDirectory().createDirectory().wasOk()) return false;
    if (! destination.exists()) return moveFile(source, destination);
    if (sameContent(source, destination)) return archive(source, backupFolder);
    return moveFile(source, uniqueDestination(destination.getParentDirectory(), destination.getFileName()));
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
        ok &= moveFile(source, uniqueDestination(backupFolder.getParentDirectory(), backupFolder.getFileName() + " leftovers"));
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

/** The names in `folder`: its files and its folders (a link to a folder
    counts as a folder). On macOS and Linux the kind comes with the listing
    itself, so a folder of thousands of sounds is listed without asking the
    disk about each one (JUCE's iterator does, three times). */
void listEntries(const File& folder, StringArray* files, StringArray* folders) {
   #if JUCE_WINDOWS
    for (const auto& entry : RangedDirectoryIterator(folder, false, "*", File::findFilesAndDirectories)) {
        auto* list = entry.isDirectory() ? folders : files;
        if (list != nullptr) list->add(entry.getFile().getFileName());
    }
   #else
    auto* directory = opendir(folder.getFullPathName().toRawUTF8());
    if (directory == nullptr) return;
    while (const auto* entry = readdir(directory)) {
        const auto name = String::fromUTF8(entry->d_name);
        if (name == "." || name == "..") continue;
        bool isFolder = entry->d_type == DT_DIR;
        if (entry->d_type == DT_LNK || entry->d_type == DT_UNKNOWN) {
            struct stat info;
            isFolder = stat(folder.getChildFile(name).getFullPathName().toRawUTF8(), &info) == 0 && S_ISDIR(info.st_mode);
        }
        auto* list = isFolder ? folders : files;
        if (list != nullptr) list->add(name);
    }
    closedir(directory);
   #endif
}

/** Visible subfolders of `folder`, in compareSoundNames() order. Reserved
    names ("Profiles", "Cartridges") only matter in the root: deeper down they
    are ordinary user folders. */
Array<File> visibleSubfolders(const File& folder, bool isLibraryRoot) {
    Array<File> found;
    StringArray names;
    listEntries(folder, nullptr, &names);
    for (const auto& name : names) {
        if (isIgnoredName(name) || (isLibraryRoot && isReservedFolderName(name))) continue;
        found.add(folder.getChildFile(name));
    }
    std::sort(found.begin(), found.end(), [](const File& a, const File& b) {
        return compareSoundNames(a.getFileName(), b.getFileName()) < 0;
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

namespace {
/** Latin letters with one accent (U+00C0 to U+017F) and the same letters
    without it, per accent, from Unicode's canonical decompositions. */
struct AccentedLetters { char16_t accent; const char16_t* letters; const char* plain; };
constexpr AccentedLetters accentedLetters[] {
    { 0x0300, u"ÀÈÌÒÙàèìòù", "AEIOUaeiou" },                                   // grave
    { 0x0301, u"ÁÉÍÓÚÝáéíóúýĆćĹĺŃńŔŕŚśŹź", "AEIOUYaeiouyCcLlNnRrSsZz" },       // acute
    { 0x0302, u"ÂÊÎÔÛâêîôûĈĉĜĝĤĥĴĵŜŝŴŵŶŷ", "AEIOUaeiouCcGgHhJjSsWwYy" },       // circumflex
    { 0x0303, u"ÃÑÕãñõĨĩŨũ", "ANOanoIiUu" },                                   // tilde
    { 0x0304, u"ĀāĒēĪīŌōŪū", "AaEeIiOoUu" },                                   // macron
    { 0x0306, u"ĂăĔĕĞğĬĭŎŏŬŭ", "AaEeGgIiOoUu" },                               // breve
    { 0x0307, u"ĊċĖėĠġİŻż", "CcEeGgIZz" },                                     // dot above
    { 0x0308, u"ÄËÏÖÜäëïöüÿŸ", "AEIOUaeiouyY" },                               // diaeresis
    { 0x030a, u"ÅåŮů", "AaUu" },                                               // ring
    { 0x030b, u"ŐőŰű", "OoUu" },                                               // double acute
    { 0x030c, u"ČčĎďĚěĽľŇňŘřŠšŤťŽž", "CcDdEeLlNnRrSsTtZz" },                   // caron
    { 0x0327, u"ÇçĢģĶķĻļŅņŖŗŞşŢţ", "CcGgKkLlNnRrSsTt" },                       // cedilla
    { 0x0328, u"ĄąĘęĮįŲų", "AaEeIiUu" },                                       // ogonek
};

struct PlainLetter { juce_wchar letter = 0, accent = 0; };

/** The plain letter and accent of each character from U+00C0 to U+017F (0: none). */
const std::array<PlainLetter, 0xc0>& plainLetters() {
    static const auto table = [] {
        std::array<PlainLetter, 0xc0> made {};
        for (const auto& group : accentedLetters)
            for (int i = 0; group.letters[i] != 0; ++i)
                made[size_t(group.letters[i] - 0xc0)] = { juce_wchar(group.plain[i]), juce_wchar(group.accent) };
        return made;
    }();
    return table;
}

bool isAccentMark(juce_wchar c) { return c >= 0x300 && c <= 0x36f; }

/** Upper case without the C library: ASCII, the Latin letters with no accent
    to take off (æ ø œ ł…), Greek and Cyrillic. */
juce_wchar foldCase(juce_wchar c) {
    if (c >= 'a' && c <= 'z') return juce_wchar(c - 0x20);
    if (c == 0xe6 || c == 0xf0 || c == 0xf8 || c == 0xfe) return juce_wchar(c - 0x20);           // æ ð ø þ
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177)) return juce_wchar(c & ~1);       // capitals even
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) return (c & 1) != 0 ? c : juce_wchar(c - 1);   // capitals odd
    if (c == 0x3c2) return 0x3a3;                                                                    // final sigma
    if ((c >= 0x3b1 && c <= 0x3c9) || (c >= 0x430 && c <= 0x44f)) return juce_wchar(c - 0x20);
    if (c >= 0x450 && c <= 0x45f) return juce_wchar(c - 0x50);
    return c;
}

/** Letters and digits, which natural order puts after everything else: past
    ASCII, all but signs, punctuation, arrows, maths, shapes, CJK punctuation
    and emoji. */
bool isLetterOrDigit(juce_wchar c) {
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return ! (c < 0xc0 || c == 0xd7 || c == 0xf7 || isAccentMark(c) || (c >= 0x2000 && c < 0x2c00)
              || (c >= 0x3000 && c < 0x3040) || (c >= 0xfe00 && c < 0xfe10) || c >= 0x1f000);
}

/** A name walked letter by letter for sorting. An accented Latin letter is
    its plain letter with the accent apart, whether it is one character or a
    letter followed by its accent. */
struct SortCursor {
    String::CharPointerType text;

    bool atEnd() const { return text.isEmpty(); }
    bool isSpace() const { const auto c = *text; return c == ' ' || (c >= 9 && c <= 13); }
    bool isDigit() const { const auto c = *text; return c >= '0' && c <= '9'; }
    juce_wchar letter() const {
        const auto c = *text;
        const auto plain = c >= 0xc0 && c < 0x180 ? plainLetters()[size_t(c - 0xc0)].letter : juce_wchar(0);
        return plain != 0 ? plain : c;
    }
    juce_wchar accent() const {
        const auto c = *text;
        if (c >= 0xc0 && c < 0x180)
            if (const auto accent = plainLetters()[size_t(c - 0xc0)].accent) return accent;
        if (c == 0) return 0;
        auto next = text;
        ++next;
        return isAccentMark(*next) ? *next : juce_wchar(0);
    }
    void advance() {
        if (atEnd()) return;
        ++text;
        while (isAccentMark(*text)) ++text;
    }
    void skipSpaces() { while (isSpace()) advance(); }
};

/** A run of digits without a leading zero: the longer one is the larger
    number, else the first different digit decides. */
int compareDigitsRight(SortCursor a, SortCursor b) {
    for (int bias = 0;; a.advance(), b.advance()) {
        const bool digitA = a.isDigit(), digitB = b.isDigit();
        if (! digitA && ! digitB) return bias;
        if (! digitA) return -1;
        if (! digitB) return 1;
        if (bias == 0 && *a.text != *b.text) bias = *a.text < *b.text ? -1 : 1;
    }
}

/** A run of digits with a leading zero: digit by digit from the left. */
int compareDigitsLeft(SortCursor a, SortCursor b) {
    for (;; a.advance(), b.advance()) {
        const bool digitA = a.isDigit(), digitB = b.isDigit();
        if (! digitA && ! digitB) return 0;
        if (! digitA) return -1;
        if (! digitB) return 1;
        if (*a.text != *b.text) return *a.text < *b.text ? -1 : 1;
    }
}

/** JUCE's String::compareNatural(…, false), step for step, on the plain
    letters: an ASCII name keeps exactly the place it had. */
int compareNaturally(SortCursor a, SortCursor b) {
    for (bool first = true;; first = false) {
        const bool spaceA = a.isSpace(), spaceB = b.isSpace();
        if (! first && spaceA != spaceB) {
            if (a.atEnd()) return -1;
            if (b.atEnd()) return 1;
            return spaceB ? 1 : -1;
        }
        a.skipSpaces();
        b.skipSpaces();
        if (a.isDigit() && b.isDigit())
            if (const auto order = *a.text == '0' || *b.text == '0' ? compareDigitsLeft(a, b) : compareDigitsRight(a, b))
                return order;
        const auto keyA = foldCase(a.letter()), keyB = foldCase(b.letter());
        if (keyA != keyB) {
            const bool wordA = isLetterOrDigit(keyA), wordB = isLetterOrDigit(keyB);
            if (wordA != wordB) return wordB ? -1 : 1;
            return keyA < keyB ? -1 : 1;
        }
        if (keyA == 0) return 0;
        a.advance();
        b.advance();
    }
}

/** Names natural order finds equal: the accents decide, then the case. */
int compareTies(SortCursor a, SortCursor b) {
    for (const bool byAccent : { true, false })
        for (auto x = a, y = b;; x.advance(), y.advance()) {
            x.skipSpaces();
            y.skipSpaces();
            if (x.atEnd() || y.atEnd()) break;
            const auto left = byAccent ? x.accent() : x.letter(), right = byAccent ? y.accent() : y.letter();
            if (left != right) return left < right ? -1 : 1;
        }
    return 0;
}
}

int compareSoundNames(const String& a, const String& b) {
    const SortCursor first { a.getCharPointer() }, second { b.getCharPointer() };
    if (const auto order = compareNaturally(first, second)) return order;
    if (const auto order = compareTies(first, second)) return order;
    return a.compare(b);   // the same name with its accents held another way
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
        ok &= ! target.exists() && moveFile(file, target);
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

#if JUCE_WINDOWS
namespace {
/** What Windows answers while another program holds a file for a moment. */
bool isHeldForAMoment(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_ACCESS_DENIED
        || error == ERROR_UNABLE_TO_REMOVE_REPLACED || error == ERROR_UNABLE_TO_MOVE_REPLACEMENT
        || error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2;
}

/** Runs `attempt` until it succeeds, again while it fails on a file held for a
    moment: about two seconds in all, the waits growing from 10 to 400 ms. Any
    other failure (a missing file, a name taken) ends it at once. */
template <typename Attempt>
bool retryWhileHeld(Attempt&& attempt) {
    for (int wait = 10, waited = 0;; wait = jmin(wait * 2, 400)) {
        if (attempt()) return true;
        if (! isHeldForAMoment(GetLastError()) || waited >= 2000) return false;
        Thread::sleep(wait);
        waited += wait;
    }
}

/** Puts the finished `replacement` in place of `target`. The old file goes to
    a hidden name until the swap is done: without one, ReplaceFileW can fail
    half-way with the old file gone and the new one still under its temporary
    name, which TemporaryFile then deletes; both would be lost. */
bool swapInto(const File& replacement, const File& target) {
    const auto from = replacement.getFullPathName(), to = target.getFullPathName();
    if (! target.exists())
        return retryWhileHeld([&] { return MoveFileExW(from.toWideCharPointer(), to.toWideCharPointer(), 0) != 0; });
    const auto kept = uniqueDestination(target.getParentDirectory(), "." + target.getFileName() + ".old");
    const auto keptPath = kept.getFullPathName();
    const bool swapped = retryWhileHeld([&] {
        if (ReplaceFileW(to.toWideCharPointer(), from.toWideCharPointer(), keptPath.toWideCharPointer(),
                         REPLACEFILE_IGNORE_MERGE_ERRORS | 4 /* REPLACEFILE_IGNORE_ACL_ERRORS */, nullptr, nullptr))
            return true;
        const auto error = GetLastError();
        // Stopped half-way (ERROR_UNABLE_TO_MOVE_REPLACEMENT_2): the old file
        // is under the hidden name; it goes back before anything else.
        if (! target.exists() && kept.existsAsFile())
            retryWhileHeld([&] { return MoveFileExW(keptPath.toWideCharPointer(), to.toWideCharPointer(), 0) != 0; });
        SetLastError(error);
        return false;
    });
    if (swapped) kept.deleteFile();
    return swapped;
}
}
#endif

bool moveFile(const File& from, const File& to) {
   #if JUCE_WINDOWS
    return retryWhileHeld([&] { return from.moveFileTo(to); });
   #else
    return from.moveFileTo(to);
   #endif
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
    if (temporary.getFile().getSize() != static_cast<int64>(data.getSize())) return false;
   #if JUCE_WINDOWS
    return swapInto(temporary.getFile(), file);
   #else
    return temporary.overwriteTargetFileWithTemporary();
   #endif
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
        // The bank's AMEM as the bank holds it now (a slot given another voice lost its own).
        const bool isAmem = bank.getAmem() != nullptr && length == size_t(AVDexedSysex::amemBytes + 8) && bytes[i + 1] == 0x43
                            && (bytes[i + 2] & 0xf0) == 0 && bytes[i + 3] == 6 && bytes[i + 4] == 0x08 && bytes[i + 5] == 0x60;
        if (isBank) { result.append(voice, SYSEX_SIZE); bankWritten = true; }
        else if (isAmem) { const auto amem = AVDexedSysex::amemMessage(bank.getAmem()); result.append(amem.data(), amem.size()); }
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

std::vector<SourceBank> readSourceBanks(const File& file) {
    std::vector<SourceBank> found;
    MemoryBlock bytes;
    if (! readImportable(file, bytes)) return found;
    const auto stem = file.getFileNameWithoutExtension();
    Cartridge plain;
    if (isPlainBankFile(bytes) && readBank(file, plain)) {
        found.push_back({ plain, readEmbeddedSettings(bytes), stem, 32 });
        return found;
    }
    // As importBank writes them: a standard file per distinct bank, read back.
    const auto contents = AVDexedSysex::scan(bytes);
    std::vector<std::pair<MemoryBlock, int>> banks;
    std::vector<const AVDexedSysex::Bank*> distinct;
    for (const auto& bank : contents.banks)
        if (std::none_of(distinct.begin(), distinct.end(), [&bank](const auto* kept) { return kept->voices == bank.voices; }))
            distinct.push_back(&bank);
    for (const auto* bank : distinct)
        banks.emplace_back(standardBankFile(*bank), 32);
    const bool singleVoices = banks.empty();
    if (singleVoices)
        banks = voicesToBankFiles(contents.voices);
    for (size_t i = 0; i < banks.size(); ++i) {
        SourceBank source;
        source.singleVoices = singleVoices;
        const int rc = source.bank.load(static_cast<const uint8_t*>(banks[i].first.getData()), int(banks[i].first.getSize()));
        if (rc != 0 && rc != 1) continue;
        source.name = banks.size() > 1 ? stem + " " + String(int(i + 1)) : stem;
        source.voices = banks[i].second;
        found.push_back(std::move(source));
    }
    return found;
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
    return moveFile(oldFile, uniqueDestination(backup, "Dexed " + stamp + ".xml"));
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
                                           : moveFile(entry, destination);
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
            ok &= moveFile(cartridges, uniqueDestination(backup, "Cartridges"));
    }

    const auto profiles = appDirectory.getChildFile("Profiles");
    if (profiles.isDirectory()) {
        const auto hidden = appDirectory.getChildFile(profilesFolderName);
        ok &= hidden.exists() ? mergeFolder(profiles, hidden, backup.getChildFile("Profiles")) : moveFile(profiles, hidden);
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
        StringArray names;
        listEntries(folder, &names, nullptr);
        Array<File> level;
        for (const auto& name : names)
            if (const auto file = folder.getChildFile(name); isSoundFile(file)) level.add(file);
        std::sort(level.begin(), level.end(), [](const File& a, const File& b) {
            return compareSoundNames(a.getFileName(), b.getFileName()) < 0;
        });
        files.addArray(level);
    };
    if (! root.isDirectory()) return files;
    addSoundsOf(root);
    forEachFolder(root, true, addSoundsOf);
    return files;
}

namespace {
constexpr const char* soundIndexHeader = "AV Dexed library index 1";
File soundIndexFile(const File& root) { return root.getChildFile(cacheFolderName).getChildFile("library-index"); }
File convertedMarker(const File& root) { return root.getChildFile(".library-presets-v1"); }
}

std::map<String, IndexedSound> readSoundIndex(const File& root) {
    std::map<String, IndexedSound> saved;
    StringArray lines;
    lines.addLines(soundIndexFile(root).loadFileAsString());
    if (lines.isEmpty() || lines[0] != soundIndexHeader) return saved;   // another format: read everything again
    // One line per file: path, size, time, order, key, name (tab separated).
    for (int i = 1; i < lines.size(); ++i) {
        const auto fields = StringArray::fromTokens(lines[i], "\t", {});
        if (fields.size() != 6 || fields[0].isEmpty() || fields[4].isEmpty()) continue;
        saved[fields[0]] = { fields[0], fields[1].getLargeIntValue(), fields[2].getLargeIntValue(), fields[3].getIntValue(),
                             fields[4], fields[5] };
    }
    return saved;
}

bool writeSoundIndex(const File& root, const std::vector<IndexedSound>& sounds) {
    // Streamed: tens of thousands of lines appended to one String copy it each time.
    MemoryOutputStream text;
    text << soundIndexHeader;
    for (const auto& sound : sounds) {
        if (sound.path.containsAnyOf("\t\r\n") || sound.name.containsAnyOf("\t\r\n")) continue;   // read again next time
        text << "\n" << sound.path << "\t" << String(sound.size) << "\t" << String(sound.modified) << "\t"
             << String(sound.order) << "\t" << sound.key << "\t" << sound.name;
    }
    text << "\n";
    const auto index = soundIndexFile(root);
    return index.getParentDirectory().createDirectory() && writeAtomically(index, text.getMemoryBlock());
}

bool isLibraryConverted(const File& root) { return convertedMarker(root).existsAsFile(); }

void setLibraryConverted(const File& root, bool converted) {
    if (converted) convertedMarker(root).replaceWithText("1\n");
    else convertedMarker(root).deleteFile();
}

std::vector<SoundFileInfo> listSoundFiles(const File& root) {
    std::vector<SoundFileInfo> found;
    for (const auto& file : findSoundFiles(root)) {
        SoundFileInfo info { file, 0, 0 };
       #if JUCE_WINDOWS
        info.size = file.getSize();
        info.modified = file.getLastModificationTime().toMilliseconds();
       #else
        // One stat for both, the time as JUCE reads it (milliseconds of the
        // modification time): the caches compare it with File's own.
        struct stat status;
        if (stat(file.getFullPathName().toRawUTF8(), &status) != 0) continue;
        info.size = int64(status.st_size);
       #if JUCE_MAC || JUCE_IOS
        info.modified = int64(status.st_mtimespec.tv_sec) * 1000 + int64(status.st_mtimespec.tv_nsec) / 1000000;
       #else
        info.modified = int64(status.st_mtime) * 1000;
       #endif
       #endif
        found.push_back(std::move(info));
    }
    return found;
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
        if (! moveFile(folder, target) || ! target.isDirectory()) return RenameResult::failed;
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
        if (! moveFile(file, target) || ! target.existsAsFile()) return RenameResult::failed;
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
        if (destination.exists() || ! moveFile(item, destination) || ! destination.exists()) return result;
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
