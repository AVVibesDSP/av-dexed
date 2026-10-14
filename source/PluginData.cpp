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

#include <time.h>

#include "PluginData.h"
#include "debugger/Logger.h"
#include "PluginProcessor.h"
#include "ui/AVDexedLanguage.h"
#include "ui/DXLookNFeel.h"

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
    applyProgram(Program(rawdata));
    if (sysexChecksum(rawdata, 155) != rawdata[155]) // rawdata[155] is a checksum in a sysex dump
        return 1; // just return 1 if the checksum doesn't match, might be normal if it is loaded from a cartridge
    return 0;
}

void DexedAudioProcessor::setupStartupCart() {
    File startup = dexedCartDir.getChildFile("Dexed_01.syx");

    if ( currentCart.load(startup) != -1 ) {
        loadCartridge(currentCart);
        activeFileCartridge = startup;
        return;
    }
    
    // The user deleted the file :/, load from the builtin zip file.
    MemoryInputStream *mis = new MemoryInputStream(BinaryData::builtin_pgm_zip, BinaryData::builtin_pgm_zipSize, false);
    ZipFile *builtin_pgm = new ZipFile(mis, true);
    InputStream *is = builtin_pgm->createStreamForEntry(builtin_pgm->getIndexOfFileName(("Dexed_01.syx")));
    Cartridge init;
    
    if ( init.load(*is) != -1 ) {
        loadCartridge(init);
    }

    delete is;
    delete builtin_pgm;
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

void DexedAudioProcessor::sendSysexCartridge(File cart) {
    if ( ! sysexComm.isOutputActive() )
        return;
    
    std::unique_ptr<juce::FileInputStream> fis = cart.createInputStream();
    if ( fis == NULL ) {
        auto& language = AVDexedLanguageService::getInstance();
        AVShowMessage(AlertWindow::WarningIcon, language.text("error"),
                      language.format("bankOpenError", {{"path", cart.getFullPathName()}}), getActiveEditor());
        return;
    }
    
    uint8 syx_data[65535];
    int sz = fis->read(syx_data, 65535);
    
    if (sz <= 0 || syx_data[0] != 0xF0) {
        auto& language = AVDexedLanguageService::getInstance();
        AVShowMessage(AlertWindow::WarningIcon, language.text("error"),
                      language.format("sysexNoData", {{"path", cart.getFullPathName()}}), getActiveEditor());
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
    dexedAppDir.createDirectory();

    const auto migrationMarker = dexedAppDir.getChildFile(".legacy-data-migration-v1");
    if (!migrationMarker.existsAsFile()) {
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
    
    dexedCartDir = dexedAppDir.getChildFile("Cartridges");

    dexedCartDir.createDirectory();
    File synprezFmDir = dexedCartDir.getChildFile("SynprezFM");
    MemoryInputStream *mis = new MemoryInputStream(BinaryData::builtin_pgm_zip,
                                                    BinaryData::builtin_pgm_zipSize, false);
    ZipFile *builtin_pgm = new ZipFile(mis, true);

    for (int i = 0; i < builtin_pgm->getNumEntries(); ++i) {
        const auto& entryName = builtin_pgm->getEntry(i)->filename;
        const auto targetDirectory = entryName == "Dexed_01.syx" ? dexedCartDir : synprezFmDir;
        const auto targetFile = targetDirectory.getChildFile(entryName);
        if (!targetFile.existsAsFile()) {
            targetDirectory.createDirectory();
            builtin_pgm->uncompressEntry(i, targetDirectory);
        }
    }
    delete builtin_pgm;
}
