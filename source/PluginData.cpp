/**
 *
 * Copyright (c) 2014-2025 Pascal Gauthier.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 */

#include "core/AVDexedLibrary.h"
#include <time.h>

#include "PluginData.h"
#include "debugger/Logger.h"
#include "PluginProcessor.h"
#include "ui/AVDexedLanguage.h"
#include "ui/DXLookNFeel.h"
#include "ui/PluginEditor.h"

#include <fstream>

namespace {

bool copyMissingFilesRecursively(const juce::File& source, const juce::File& destination) {
    if (!source.isDirectory())
        return true;

    bool copied = destination.createDirectory().wasOk();
    for (const auto& entry : juce::RangedDirectoryIterator(source, true, "*", juce::File::findFiles)) {
        const auto sourceFile = entry.getFile();
        if (sourceFile.getFileName() == ".legacy-data-migration-v1")
            continue;
        const auto destinationFile = destination.getChildFile(sourceFile.getRelativePathFrom(source));
        if (!destinationFile.existsAsFile()) {
            copied &= destinationFile.getParentDirectory().createDirectory().wasOk()
                && sourceFile.copyFileTo(destinationFile);
        }
    }
    return copied;
}

} // namespace

void DexedAudioProcessor::loadCartridge(const Cartridge& sysex) {
    currentCart = sysex;
    currentCart.getProgramNames(programNames);
    programChanged.set(true);
}

// void DexedAudioProcessor::packOpSwitch() {
//     char value = (controllers.opSwitch[5] == '1') << 5;
//     value += (controllers.opSwitch[4] == '1') << 4;
//     value += (controllers.opSwitch[3] == '1') << 3;
//     value += (controllers.opSwitch[2] == '1') << 2;
//     value += (controllers.opSwitch[1] == '1') << 1;
//     value += (controllers.opSwitch[0] == '1');
//     //data[155] = value;
// }
//
// void DexedAudioProcessor::unpackOpSwitch(char packOpValue) {
//     controllers.opSwitch[5] = ((packOpValue >> 5) &1) + 48;
//     controllers.opSwitch[4] = ((packOpValue >> 4) &1) + 48;
//     controllers.opSwitch[3] = ((packOpValue >> 3) &1) + 48;
//     controllers.opSwitch[2] = ((packOpValue >> 2) &1) + 48;
//     controllers.opSwitch[1] = ((packOpValue >> 1) &1) + 48;
//     controllers.opSwitch[0] = (packOpValue &1) + 48;
// }

int DexedAudioProcessor::updateProgramFromSysex(const uint8_t *rawdata) {
    // The same rule as a bank voice (Cartridge::unpackProgram): Dexed 1.1.0
    // took these bytes as they came, so an algorithm or pitch envelope value out
    // of range reached the engine's tables.
    Program voice(rawdata);
    Cartridge::applyBankRules(voice);
    applyProgram(voice);
    if (sysexChecksum(rawdata, 155) != rawdata[155]) // rawdata[155] is a checksum in a sysex dump
        return 1; // just return 1 if the checksum doesn't match, might be normal if it is loaded from a cartridge
    return 0;
}

void DexedAudioProcessor::setupStartupCart() {
    const File startup = AVDexedLibrary::folderNamed(dexedCartDir, AVDexedLibrary::factoryFolderName).getChildFile("Dexed_01.syx");

    Cartridge bank;
    if (AVDexedLibrary::readBank(startup, bank)) {
        setActiveBank(bank, startup);
        return;
    }

    // The user deleted the file :/, load from the builtin zip file: a bank
    // without file (no settings, and "Guardar" goes to "Guardar como…").
    MemoryInputStream zipSource(BinaryData::builtin_pgm_zip, BinaryData::builtin_pgm_zipSize, false);
    ZipFile builtin(zipSource);
    std::unique_ptr<InputStream> stream(builtin.createStreamForEntry(builtin.getIndexOfFileName("Dexed_01.syx")));
    Cartridge init;
    if (stream != nullptr && init.load(*stream) != -1)
        setActiveBank(init, File());
}

//==============================================================================
DexedAudioProcessor::AVSoundTable DexedAudioProcessor::readAVSoundTable(const File& bankFile) const {
    // A bank without file, or one that cannot be read, has no settings: every
    // sound plays with the defaults.
    if (bankFile == File() || ! bankFile.existsAsFile())
        return {};
    return makeAVSoundTable(AVDexedLibrary::readEmbeddedSettings(bankFile));
}

void DexedAudioProcessor::installAVActiveBank(const Cartridge& bank, const File& file, const AVSoundTable& table) {
    // The audio thread reads currentCart and the table on program changes:
    // swap them together, briefly, under the callback lock.
    const ScopedLock audioLock(getCallbackLock());
    loadCartridge(bank);
    activeFileCartridge = file;
    {
        const SpinLock::ScopedLockType tableLock(avSoundTableLock);
        avSoundTable = table;
    }
    programChanged.set(true);
}

void DexedAudioProcessor::setActiveBank(const Cartridge& bank, const File& file) {
    // File access first, outside every lock.
    const auto table = readAVSoundTable(file);
    installAVActiveBank(bank, file, table);
}

namespace {

/** True for a .syx that lives in a visible folder of the library. */
bool isAVLibraryBankFile(const File& file, const File& root) {
    if (root == File() || ! file.existsAsFile() || ! file.isAChildOf(root))
        return false;
    for (auto item = file; item != root && item.isAChildOf(root); item = item.getParentDirectory())
        if (AVDexedLibrary::isIgnoredName(item.getFileName()))
            return false;
    return true;
}

/** The library lock for one save; `held` is false when another AV Dexed kept it too long. */
struct AVScopedLibraryLock {
    AVDexedLibrary::LibraryScope scope { 3000 };
    const bool held = scope.held;
};

/** A typed name as a DX7 name (10 ASCII characters): accented letters keep
    their base letter ("Canción" → "Cancion") instead of becoming spaces. */
String avDX7Name(const String& requested) {
    static const String accented(CharPointer_UTF8("\xc3\x81\xc3\x80\xc3\x82\xc3\x84\xc3\x83\xc3\xa1\xc3\xa0\xc3\xa2\xc3\xa4\xc3\xa3"   // ÁÀÂÄÃáàâäã
                                                  "\xc3\x89\xc3\x88\xc3\x8a\xc3\x8b\xc3\xa9\xc3\xa8\xc3\xaa\xc3\xab"                   // ÉÈÊËéèêë
                                                  "\xc3\x8d\xc3\x8c\xc3\x8e\xc3\x8f\xc3\xad\xc3\xac\xc3\xae\xc3\xaf"                   // ÍÌÎÏíìîï
                                                  "\xc3\x93\xc3\x92\xc3\x94\xc3\x96\xc3\x95\xc3\xb3\xc3\xb2\xc3\xb4\xc3\xb6\xc3\xb5"   // ÓÒÔÖÕóòôöõ
                                                  "\xc3\x9a\xc3\x99\xc3\x9b\xc3\x9c\xc3\xba\xc3\xb9\xc3\xbb\xc3\xbc"                   // ÚÙÛÜúùûü
                                                  "\xc3\x91\xc3\xb1\xc3\x87\xc3\xa7"));                                                // ÑñÇç
    static const String plain("AAAAAaaaaaEEEEeeeeIIIIiiiiOOOOOoooooUUUUuuuuNnCc");
    jassert(accented.length() == plain.length());
    String name;
    for (const auto c : requested) {
        const int index = accented.indexOfChar(c);
        name << (index >= 0 ? String::charToString(plain[index]) : String::charToString(c));
    }
    return name.substring(0, 10);
}

/** What a slot plays with, by the rule of applyAVSoundSettings(): its own
    entry while it still holds that voice, else the first entry with the same
    voice (moved or duplicated in the bank), else the defaults (empty). */
std::map<String, float> avSettingsHeard(const AVDexedLibrary::BankSettings& settings, juce::uint64 voice, int slot) {
    if (const auto own = settings.find(slot); own != settings.end() && own->second.voice == voice)
        return own->second.values;
    for (const auto& [index, entry] : settings)
        if (entry.voice == voice)
            return entry.values;
    return {};
}

} // namespace

bool DexedAudioProcessor::packAVVoiceIfChanged(Cartridge& bank, int slot, const Program& voice) {
    // An unchanged voice keeps the bank's own bytes (and the favourites keyed
    // by them): only a real edit or a new name repacks the slot. The name is
    // the voice's own, so this matches what "Guardar" writes.
    const auto stored = bank.unpackProgram(slot);
    if (std::memcmp(stored.getRawData(), voice.getRawData(), Program::PROGRAM_SIZE + 1) == 0)
        return true;
    char name[11] {};
    for (int i = 0; i < 10; ++i)
        name[i] = static_cast<char>(voice.getRawData()[145 + i] & 0x7f);
    return bank.packProgram(voice.getRawData(), slot, String(CharPointer_ASCII(name)));
}

bool DexedAudioProcessor::canSaveAVSoundToBank() const {
    const auto root = AVDexedLibrary::rootFor(dexedAppDir);
    // A locked file or folder goes to "Guardar como…" rather than to an error.
    return currentCart.containsData()
        && avSoundFromBankSlot.load(std::memory_order_relaxed)
        && isAVLibraryBankFile(activeFileCartridge, root)
        && ! AVDexedLibrary::isReadOnly(activeFileCartridge, root)
        && activeFileCartridge.hasWriteAccess()
        && activeFileCartridge.getParentDirectory().hasWriteAccess();
}

DexedAudioProcessor::AVBankSaveResult DexedAudioProcessor::saveAVSoundToBank(const String& name) {
    if (! canSaveAVSoundToBank())
        return AVBankSaveResult::needsSaveAs;

    // What is playing now. The audio thread may change the program (MIDI), so
    // take the slot, its voice and its settings together.
    int slot = 0;
    Program voice;
    AVDexedLibrary::SlotSettings modernSettings;
    {
        const ScopedLock audioLock(getCallbackLock());
        slot = juce::jlimit(0, 31, currentProgram);
        voice = activeProgram;
        modernSettings = captureAVSoundSettings();
    }
    // An empty name, or the name it already has, keeps the voice's own name
    // bytes: the name shown is normalised, and writing it back would count as
    // an edit and repack an untouched voice.
    const auto requestedName = avDX7Name(name);
    const bool renamed = requestedName.trimEnd().isNotEmpty() && requestedName.trimEnd() != voice.getName().trimEnd();
    if (renamed)
        voice.setName(requestedName);
    const auto file = activeFileCartridge;

    AVDexedLibrary::BankSettings settings;
    Cartridge savedBank;
    {
        AVScopedLibraryLock lock;
        if (! lock.held)
            return AVBankSaveResult::failed;

        // The file must still hold the bank that is loaded: another AV Dexed,
        // Finder or the bank manager may have changed it since. Writing our
        // copy would then replace the other 31 sounds with old ones.
        MemoryBlock original;
        if (! file.existsAsFile() || file.getSize() <= 0 || file.getSize() >= 65536)
            return AVBankSaveResult::needsSaveAs;
        if (! file.loadFileAsData(original))
            return AVBankSaveResult::failed;
        Cartridge onDisk;
        const int loadResult = onDisk.load(static_cast<const uint8_t*>(original.getData()), static_cast<int>(original.getSize()));
        if ((loadResult != 0 && loadResult != 1)
            || AVDexedLibrary::bankFingerprint(onDisk) != AVDexedLibrary::bankFingerprint(currentCart))
            return AVBankSaveResult::needsSaveAs;

        // Only this slot changes, and its bytes only when the voice itself
        // changed: with just new settings the bank (and its favourites) stays.
        savedBank = onDisk;
        if (! packAVVoiceIfChanged(savedBank, slot, voice))
            return AVBankSaveResult::failed;

        // Settings follow their voice by hash, so replacing this slot's entry
        // could change what a duplicate of its voice, or a voice the bank
        // manager moved away from this slot, finds. Every other slot must play
        // exactly as before and this one as captured: a slot that would change
        // gets an entry of its own (empty = defaults). A pinned slot keeps its
        // own entry, so this ends after at most 32 pins.
        settings = AVDexedLibrary::readEmbeddedSettings(original);
        std::array<juce::uint64, 32> voices {};
        std::array<std::map<String, float>, 32> heard;
        for (int index = 0; index < 32; ++index) {
            voices[static_cast<size_t>(index)] = AVDexedLibrary::voiceHash(savedBank, index);
            heard[static_cast<size_t>(index)] = avSettingsHeard(settings, voices[static_cast<size_t>(index)], index);
        }
        heard[static_cast<size_t>(slot)] = modernSettings.values;
        if (modernSettings.values.empty())
            settings.erase(slot);
        else
            settings[slot] = { voices[static_cast<size_t>(slot)], modernSettings.values };
        for (bool pinned = true; pinned;) {
            pinned = false;
            for (int index = 0; index < 32; ++index) {
                const auto i = static_cast<size_t>(index);
                if (avSettingsHeard(settings, voices[i], index) != heard[i]) {
                    settings[index] = { voices[i], heard[i] };
                    pinned = true;
                }
            }
        }

        const auto bytes = AVDexedLibrary::composeBankFile(original, savedBank, settings);
        if (! AVDexedLibrary::writeAtomically(file, bytes))
            return AVBankSaveResult::failed;
    }

    // The bank in memory is now the one on disk; the program stays.
    installAVActiveBank(savedBank, file, makeAVSoundTable(settings));
    if (renamed) {
        const ScopedLock audioLock(getCallbackLock());
        if (currentProgram == slot)
            activeProgram.setName(requestedName);
    }
    // The sound is that bank voice now (as if it had just been loaded).
    avPresetContextCurrent.store(false, std::memory_order_relaxed);
    avKnownPresetOrigin.store(true, std::memory_order_relaxed);
    captureAVPresetBaseline();
    programChanged.set(true);
    return AVBankSaveResult::saved;
}

void DexedAudioProcessor::resetToInitVoice() {
    applyProgram(Program::initialProgram());
}

void DexedAudioProcessor::copyToClipboard(int srcOp) {
    DexedClipboard clipboard(activeProgram.getParameters(srcOp *21), 21);
    clipboard.write(String("Program: '") + getProgramName(getCurrentProgram()) + "' operator: " + String(6-srcOp));
}

static void setParam(DexedApvts &apvts, const String &id, float value) {
    if ( auto *p = apvts.getParameter(id) ) {
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1(value));
        p->endChangeGesture();
    }
}

void DexedAudioProcessor::pasteOpFromClipboard(int destOp) {
    DexedClipboard clipboard;
    if ( !clipboard.isOperatorData() )
        return;

    pasteEnvFromClipboard(destOp);

    const uint8_t *d = clipboard.getRawData();
    String op = "op" + String(6 - destOp);
    static const char *names[] = { "breakpoint", "lScaleDepth", "rScaleDepth", "lKeyScale", "rKeyScale",
        "rateScaling", "ampModeSens", "keyVelocity", "outputLevel", "mode", "frequencyCoarse",
        "frequencyFine", "detune" };

    for (int k = 0; k < 13; k++)
        setParam(parameters, op + names[k], d[8+k] - (k == 12 ? 7 : 0)); // detune display offset is 7
}

void DexedAudioProcessor::pasteEnvFromClipboard(int destOp) {
    DexedClipboard clipboard;
    if ( !clipboard.isOperatorData() )
        return;

    const uint8_t *d = clipboard.getRawData();
    String op = "op" + String(6 - destOp);

    for (int j = 0; j < 4; j++) {
        setParam(parameters, op + "egRate" + String(j+1), d[j]);
        setParam(parameters, op + "egLevel" + String(j+1), d[4+j]);
    }
}

void DexedAudioProcessor::sendCurrentSysexProgram() {
    if ( sysexComm.isOutputActive() ) {
        sysexComm.send(activeProgram.toMidiMessage(sysexComm.getChl()));
    }
}

void DexedAudioProcessor::sendCurrentSysexCartridge() {
    uint8_t raw[4104];

    currentCart.saveVoice(raw);
    raw[2] = raw[2] | sysexComm.getChl();
    if ( sysexComm.isOutputActive() ) {
        sysexComm.send(MidiMessage(raw, 4104));
    }
}

namespace {

/** A SysEx error as one of the editor's own in-editor notices when an editor is
    open (they live on the message thread), else the AV-themed alert. */
void avShowSysExError(AudioProcessorEditor* editor, const String& message) {
    auto& language = AVDexedLanguageService::getInstance();
    if (MessageManager::existsAndIsCurrentThread()) {
        if (auto* dexedEditor = dynamic_cast<DexedAudioProcessorEditor*>(editor)) {
            dexedEditor->showAVNotice(language.text("error"), message);
            return;
        }
    }
    AVShowMessage(AlertWindow::WarningIcon, language.text("error"), message, editor);
}

} // namespace

void DexedAudioProcessor::sendSysexCartridge(File cart) {
    if ( ! sysexComm.isOutputActive() )
        return;

    std::unique_ptr<juce::FileInputStream> fis = cart.createInputStream();
    if ( fis == NULL ) {
        avShowSysExError(getActiveEditor(), AVDexedLanguageService::getInstance()
                                                .format("bankOpenError", {{"path", cart.getFullPathName()}}));
        return;
    }

    uint8 syx_data[65535];
    int sz = fis->read(syx_data, 65535);

    if (sz <= 0 || syx_data[0] != 0xF0) {
        avShowSysExError(getActiveEditor(), AVDexedLanguageService::getInstance()
                                                .format("sysexNoData", {{"path", cart.getFullPathName()}}));
        return;
    }
    sysexComm.send(MidiMessage(syx_data, sz));
}

File DexedAudioProcessor::dexedAppDir;
File DexedAudioProcessor::dexedCartDir;

void DexedAudioProcessor::resolvAppDir() {
    const auto executable = File::getSpecialLocation(File::currentExecutableFile);
    Array<File> legacyAppDirs;

    // Preserve the original portable layouts as migration sources. The new
    // writable location is product-scoped under AV Vibes. Existing legacy data is copied, never moved or
    // deleted, so other installations remain untouched.
    #if JUCE_MAC || JUCE_IOS
        const auto portableLegacy = executable.getParentDirectory().getParentDirectory()
                                             .getParentDirectory().getSiblingFile("Dexed");
    #else
        const auto portableLegacy = executable.getSiblingFile("Dexed");
    #endif
    if (portableLegacy.isDirectory())
        legacyAppDirs.addIfNotAlreadyThere(portableLegacy);

    #if JUCE_MAC || JUCE_IOS
        const File platformDataRoot("~/Library/Application Support");
    #elif JUCE_WINDOWS
        const auto platformDataRoot = File::getSpecialLocation(File::userApplicationDataDirectory);
    #else
        const auto xdgDataHome = juce::SystemStats::getEnvironmentVariable("XDG_DATA_HOME", {});
        const auto platformDataRoot = xdgDataHome.isNotEmpty() && File::isAbsolutePath(xdgDataHome)
            ? File(xdgDataHome) : File("~/.local/share");
    #endif
    const auto legacyDigitalSuburbanDir = platformDataRoot.getChildFile("DigitalSuburban").getChildFile("Dexed");
    legacyAppDirs.addIfNotAlreadyThere(legacyDigitalSuburbanDir);
    legacyAppDirs.addIfNotAlreadyThere(File::getSpecialLocation(File::userApplicationDataDirectory)
                                         .getChildFile("DigitalSuburban").getChildFile("Dexed"));

    dexedAppDir = platformDataRoot.getChildFile("AV Vibes").getChildFile("AV Dexed");
   #if JUCE_DEBUG || defined(AVDEXED_TESTING)
    // Native QA runs against a throwaway data folder, never the real one.
    const auto overrideDirectory = SystemStats::getEnvironmentVariable("AVDEXED_DATA_DIR", {});
    if (overrideDirectory.isNotEmpty() && File::isAbsolutePath(overrideDirectory))
        dexedAppDir = File(overrideDirectory);
   #endif
    dexedAppDir.createDirectory();

    const auto libraryMarker = dexedAppDir.getChildFile(".library-layout-v2");
    const auto migrationMarker = dexedAppDir.getChildFile(".legacy-data-migration-v1");
    // A library already in the 0.2.0 layout never pulls old banks in again
    // (the user may have deleted them on purpose).
    if (!migrationMarker.existsAsFile() && !libraryMarker.existsAsFile()) {
        bool migrationSucceeded = true;
        // Early local builds used JUCE's generic app-data location, which on
        // macOS is ~/Library rather than Application Support. Copy that fork
        // data too, including native presets, without transferring its marker.
        const auto earlyForkDirectory = File::getSpecialLocation(File::userApplicationDataDirectory)
                                            .getChildFile("AV Vibes").getChildFile("AV Dexed");
        if (earlyForkDirectory != dexedAppDir)
            migrationSucceeded &= copyMissingFilesRecursively(earlyForkDirectory, dexedAppDir);
        const auto newSettings = dexedAppDir.getChildFile("Dexed.xml");
        for (const auto& legacyDir : legacyAppDirs) {
            if (legacyDir == dexedAppDir)
                continue;

            if (!newSettings.existsAsFile()) {
                const auto oldSettings = legacyDir.getChildFile("Dexed.xml");
                if (oldSettings.existsAsFile())
                    migrationSucceeded &= oldSettings.copyFileTo(newSettings);
            }

            // Very old releases kept this file one directory above the data folder.
            if (!newSettings.existsAsFile()) {
                const auto oldParentSettings = legacyDir.getParentDirectory().getChildFile("Dexed.xml");
                if (oldParentSettings.existsAsFile())
                    migrationSucceeded &= oldParentSettings.copyFileTo(newSettings);
            }

            migrationSucceeded &= copyMissingFilesRecursively(legacyDir.getChildFile("Cartridges"),
                                                              dexedAppDir.getChildFile("Cartridges"));
        }
        if (migrationSucceeded)
            migrationMarker.replaceWithText("completed");
        else
            DXLOG("Legacy data migration incomplete; original files retained and copy will retry on next launch");
    }
    
    // 0.2.0: the preferences file no longer carries the original plugin's name.
    if (!AVDexedLibrary::migratePreferences(dexedAppDir, preferencesFileName))
        DXLOG("Preferences rename incomplete; Dexed.xml kept and the merge will retry on next launch");

    // 0.2.0: the data folder is the library; its folders are the plugin's folders.
    if (!AVDexedLibrary::migrateLayout(dexedAppDir, BinaryData::builtin_pgm_zip, (size_t) BinaryData::builtin_pgm_zipSize))
        DXLOG("Library layout migration incomplete; nothing was deleted and it will retry on next launch");

    dexedCartDir = AVDexedLibrary::rootFor(dexedAppDir);
}
