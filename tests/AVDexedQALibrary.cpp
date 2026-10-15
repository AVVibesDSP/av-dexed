// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// Native QA of the library on a COPY of a real data folder: the plugin starts
// as in a DAW (user-data mode, so the 0.2.0 migration runs), restores the
// standalone session saved in that folder, opens the editor, lets the browser
// index and the external banks be adopted, renders the Play view and prints
// what is on disk. Run with
//   CFFIXED_USER_HOME=<copy home> AVDEXED_DATA_DIR=<copy home>/Library/Application Support/AV Vibes/AV Dexed
//   AVDexedRegressionTests --qa-library <output folder>
// It refuses to run on anything under the real home's Library.
// On Windows: AVDEXED_DATA_DIR=<copy outside %APPDATA%> AVDexedRegressionTests --qa-library <output folder>;
// anything in (or containing) %APPDATA% is refused.

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "../source/ui/PluginEditor.h"
#include "../source/core/AVDexedLibrary.h"
#include <cstdlib>
#include <functional>
#include <iostream>

bool AVRunLibraryQA(const juce::File& outputDirectory) {
    using namespace juce;
    const auto dataDirectory = SystemStats::getEnvironmentVariable("AVDEXED_DATA_DIR", {});
#if JUCE_WINDOWS
    // No CFFIXED_USER_HOME here: the real data lives in %APPDATA%\AV Vibes\AV Dexed,
    // and %APPDATA% (like ~/Library) also holds the original Dexed's data.
    const auto realAppData = File::getSpecialLocation(File::userApplicationDataDirectory);
    const auto realVendorFolder = realAppData.getChildFile("AV Vibes");
    bool refused = dataDirectory.isEmpty() || ! File::isAbsolutePath(dataDirectory);
    if (! refused) {
        const File copy(dataDirectory);
        // Only a plain local "X:\..." path: no UNC or \\?\ forms, no drive-relative
        // "C:folder", no part Win32 would trim ("Roaming." names Roaming).
        const auto full = copy.getFullPathName();
        refused = ! (full.length() >= 3 && full[1] == ':' && full[2] == '\\');
        for (const auto& part : StringArray::fromTokens(full, "\\", ""))
            refused = refused || part.endsWithChar('.') || part.endsWithChar(' ');
        // Junctions and links (also Windows' own, e.g. "Application Data") may lead
        // into %APPDATA% and cannot always be resolved: refuse any on the way. Short
        // names: compare where the folder (or its nearest existing parent) really is.
        auto existing = copy;
        while (! existing.isDirectory() && existing.getParentDirectory() != existing)
            existing = existing.getParentDirectory();
        bool throughLink = false;
        for (auto part = existing; part.getParentDirectory() != part; part = part.getParentDirectory())
            throughLink |= part.isSymbolicLink();
        const auto onDisk = existing.getLinkedTarget(), appDataOnDisk = realAppData.getLinkedTarget();
        refused = refused || throughLink || ! existing.isOnHardDisk()   // mapped network drives are not resolved
               || copy == realAppData || copy.isAChildOf(realAppData) || realAppData.isAChildOf(copy)
               || onDisk == appDataOnDisk || onDisk.isAChildOf(appDataOnDisk)
               || (existing == copy && appDataOnDisk.isAChildOf(onDisk));
    }
    if (refused) {
        std::cerr << "QA refused: AVDEXED_DATA_DIR must be a plain local folder outside the real %APPDATA%, without links.\n";
        return false;
    }
    outputDirectory.createDirectory();
    // The copy's saved paths still name the real folder; point them at the copy.
    const auto realData = realVendorFolder.getChildFile("AV Dexed").getFullPathName();
#else
#if ! JUCE_MAC
    // The guard below only knows ~/Library; Linux keeps its data elsewhere.
    std::cerr << "QA refused: --qa-library runs on macOS and Windows only.\n";
    return false;
#endif
    const char* realHome = std::getenv("HOME");
    if (dataDirectory.isEmpty() || ! File::isAbsolutePath(dataDirectory) || realHome == nullptr
        || File(dataDirectory).isAChildOf(File(String(realHome)).getChildFile("Library"))) {
        std::cerr << "QA refused: AVDEXED_DATA_DIR must point at a copy outside the real ~/Library.\n";
        return false;
    }
    outputDirectory.createDirectory();
    // The copy's saved paths still name the real folder; in real use they name
    // the data folder itself, so point them at the copy (never at the original).
    const auto realData = String(realHome) + "/Library/Application Support/AV Vibes/AV Dexed";
#endif
    const auto copiedPreferences = File(dataDirectory).getChildFile("Dexed.xml");
    if (copiedPreferences.existsAsFile())
        copiedPreferences.replaceWithText(copiedPreferences.loadFileAsString().replace(realData, dataDirectory));
    bool passed = true;
    {
        DexedAudioProcessor processor;   // user-data mode: resolvAppDir() migrates the copy
        std::cout << "Data folder: " << DexedAudioProcessor::dexedAppDir.getFullPathName() << '\n';
        if (DexedAudioProcessor::dexedAppDir != File(dataDirectory)) {
            std::cerr << "The data folder override was not used.\n";
            return false;
        }
        processor.setRateAndBufferSizeDetails(48000.0, 64);
        processor.prepareToPlay(48000.0, 64);
        // The standalone keeps the last session next to the library: restore it
        // like the standalone does (paths saved by 0.1.0 point into Cartridges/).
        PropertiesFile::Options readOnly;
        readOnly.millisecondsBeforeSaving = -1;
        PropertiesFile standalone(DexedAudioProcessor::dexedAppDir.getChildFile("AV Dexed.settings"), readOnly);
        MemoryBlock session;
        if (session.fromBase64Encoding(standalone.getValue("filterState")) && session.getSize() > 0) {
            if (auto xml = AudioProcessor::getXmlFromBinary(session.getData(), static_cast<int>(session.getSize())))
                if (auto pointed = parseXML(xml->toString().replace(realData, dataDirectory))) {
                    session.reset();
                    AudioProcessor::copyXmlToBinary(*pointed, session);
                }
            processor.setStateInformation(session.getData(), static_cast<int>(session.getSize()));
            std::cout << "Restored session: " << processor.getAVPresetName() << " · bank "
                      << processor.activeFileCartridge.getFullPathName() << " · program "
                      << processor.getCurrentProgram() + 1 << '\n';
        } else {
            std::cout << "No standalone session to restore.\n";
        }
        auto editor = std::unique_ptr<DexedAudioProcessorEditor>(
            dynamic_cast<DexedAudioProcessorEditor*>(processor.createEditor()));
        if (editor == nullptr) return false;
        editor->setVisible(true);
        const auto deadline = Time::getMillisecondCounterHiRes() + 15000;
        const auto settleUntil = Time::getMillisecondCounterHiRes() + 3000;   // adoption + re-index
        std::function<void()> finish;
        bool completed = false;
        finish = [&] {
            const auto now = Time::getMillisecondCounterHiRes();
            if ((editor->playView.libraryIndexing || now < settleUntil) && now < deadline) {
                Timer::callAfterDelay(100, finish);
                return;
            }
            const auto& entries = editor->playView.libraryEntries;
            std::cout << "Indexed sounds: " << entries.size() << '\n';
            passed &= ! editor->playView.libraryIndexing && entries.size() > 32;
            PropertiesFile::Options options;
            options.millisecondsBeforeSaving = -1;
            PropertiesFile preferences(DexedAudioProcessor::getPreferencesFile(), options);
            const auto favourites = StringArray::fromLines(preferences.getValue("favoritePresetIds"));
            for (const auto& key : favourites) {
                if (key.isEmpty()) continue;
                const auto found = std::find_if(entries.begin(), entries.end(), [&key](const auto& entry) { return entry.key == key; });
                std::cout << "Favourite " << key << ": "
                          << (found != entries.end() ? (found->name + " (" + found->source + ")").toStdString() : std::string("NOT FOUND")) << '\n';
                passed &= found != entries.end();
            }
            const auto& visible = editor->playView.visibleEntries;
            std::cout << "Active bank now: " << processor.activeFileCartridge.getFullPathName()
                      << " · modified " << (processor.isAVPresetModified() ? "yes" : "no")
                      << " · selected row " << editor->playView.programList.getSelectedRow()
                      << " · first visible row " << editor->playView.programList.getRowContainingPosition(10, 10) << '\n';
            for (size_t i = 0; i < visible.size() && i < 3; ++i)
                std::cout << "  row " << i << ": " << visible[i].name << " · " << visible[i].source << " · folder '"
                          << visible[i].folder << "'" << (visible[i].transient ? " · transient" : "")
                          << (editor->playView.isActiveEntry(visible[i]) ? " · ACTIVE" : "") << '\n';
            int transientCount = 0;
            for (const auto& entry : entries) transientCount += entry.transient ? 1 : 0;
            std::cout << "Transient entries: " << transientCount << '\n';
            std::cout << "Pending external banks: " << preferences.getValue("presetBrowserKnownBanks").replace("\n", " | ") << '\n';
            std::cout << "Selected folder: " << preferences.getValue("presetBrowserSelectedFolder") << '\n';
            const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
            PNGImageFormat png;
            const auto file = outputDirectory.getChildFile("QA-Play.png");
            file.deleteFile();
            if (auto stream = file.createOutputStream()) png.writeImageToStream(image, *stream);
            std::cout << "Rendered " << file.getFullPathName() << '\n';
            // The chorus settings over the Play view: the whole editor behind it
            // must be blurred, the top bar included.
            if (auto& caption = editor->playView.knobCaptions[5]) editor->playView.showEffectSettings(2, *caption);
            if (auto& overlay = editor->playView.effectSettingsWindow) {
                // It must start transparent and fade in, not pop up.
                std::cout << "Effect alpha right after opening: " << overlay->getAlpha() << '\n';
                passed &= overlay->getAlpha() < 0.5f;
            }
            Timer::callAfterDelay(400, [&] {
                const auto withEffect = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
                const auto effectFile = outputDirectory.getChildFile("QA-Effect.png");
                effectFile.deleteFile();
                if (auto stream = effectFile.createOutputStream()) PNGImageFormat().writeImageToStream(withEffect, *stream);
                std::cout << "Rendered " << effectFile.getFullPathName() << '\n';
                completed = true;
#if !JUCE_MAC
                MessageManager::getInstance()->stopDispatchLoop();
#endif
            });
        };
        Timer::callAfterDelay(200, finish);
#if JUCE_MAC
        // Console app: no NSApp run loop. Pump the CF run loop JUCE's messages use
        // (as the UI evidence does) until the checks above have run.
        const auto hardDeadline = Time::getMillisecondCounterHiRes() + 30000;
        while (! completed && Time::getMillisecondCounterHiRes() < hardDeadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
#else
        MessageManager::getInstance()->runDispatchLoop();
#endif
        passed &= completed;
        editor.reset();
        processor.releaseResources();
    }
    std::cout << "Library on disk (hidden folders included):\n";
    Array<File> files;
    DexedAudioProcessor::dexedAppDir.findChildFiles(files, File::findFilesAndDirectories, true, "*");
    files.sort();
    for (const auto& file : files)
        std::cout << "  " << file.getRelativePathFrom(DexedAudioProcessor::dexedAppDir)
                  << (file.isDirectory() ? "/" : "") << '\n';
    std::cout << (passed ? "Library QA passed.\n" : "Library QA FAILED.\n");
    return passed;
}
