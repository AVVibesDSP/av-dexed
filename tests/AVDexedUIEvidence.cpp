// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "../source/ui/PluginEditor.h"
#include "../source/ui/window/AVDexedSettingsPanel.h"
#include "../source/ui/window/AboutBox.h"
#include "../source/ui/window/TuningShow.h"
#include <iostream>

// Visual QA of the real editor. These PNGs are
// evidence only: no raster data is loaded or embedded in the instrument UI.
bool AVRenderUIEvidence(const juce::File& directory) {
    using namespace juce;
    directory.createDirectory();
    const auto oldAppDirectory = DexedAudioProcessor::dexedAppDir;
    const auto oldCartDirectory = DexedAudioProcessor::dexedCartDir;
    const auto oldLanguage = AVDexedLanguageService::getInstance().getLanguage();
    DexedAudioProcessor::dexedAppDir = directory.getChildFile("isolated-preferences");
    DexedAudioProcessor::dexedCartDir = DexedAudioProcessor::dexedAppDir.getChildFile("Cartridges");
    DexedAudioProcessor::dexedCartDir.createDirectory();
    bool passed = true;
    bool completed = false;
    {
        DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
        MemoryInputStream zipSource(BinaryData::builtin_pgm_zip, BinaryData::builtin_pgm_zipSize, false);
        ZipFile zip(&zipSource, false);
        if (auto stream = std::unique_ptr<InputStream>(zip.createStreamForEntry(zip.getIndexOfFileName("Dexed_01.syx")))) {
            Cartridge bank;
            if (bank.load(*stream) == 0) { processor.loadCartridge(bank); processor.setCurrentProgram(4); }
        }
        processor.setRateAndBufferSizeDetails(48000.0, 64);
        processor.prepareToPlay(48000.0, 64);
        processor.setAVPresetContext(String::fromUTF8("Órbita oceánica – AV"), "visual-evidence", {});
        processor.captureAVPresetBaseline();
        MemoryBlock initial;
        processor.getStateInformation(initial);
        auto editor = std::make_unique<DexedAudioProcessorEditor>(&processor);
        processor.keyboardState.noteOn(2, 60, 0.8f); // unrelated incoming MIDI must survive UI changes
        Timer::callAfterDelay(2200, [&] {
        passed &= !editor->playView.libraryIndexing && editor->playView.libraryEntries.size() > 32;
        std::cout << "Indexed presets: " << editor->playView.libraryEntries.size() << '\n';
        const auto checkBounds = [&](Component& component) {
            for (auto* child : component.getChildren()) {
                if (!child->isVisible()) continue;
                if (!component.getLocalBounds().contains(child->getBounds())) {
                    std::cerr << "Clipped control: " << child->getName() << " " << child->getBounds().toString() << '\n';
                    passed = false;
                }
            }
        };
        for (auto locale : {"es", "en"}) {
            AVDexedLanguageService::getInstance().setLanguage(locale);
            AVDexedLanguageService::getInstance().sendSynchronousChangeMessage();
            editor->applyLanguage();
            for (float scale : {1.0f, 0.8f}) {
                processor.setZoomFactor(scale);
                editor->resetSize();
                for (bool play : {true, false}) {
                    editor->showAVDexedPlayView(play, false);
                    editor->refreshPresetHeader();
                    for (auto& op : editor->operators) checkBounds(*op);
                    checkBounds(editor->global);
                    auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
                    const auto name = String(play ? "Play-" : "Advanced-") + locale + (scale == 1.0f ? "-normal.png" : "-compact.png");
                    auto output = directory.getChildFile(name).createOutputStream();
                    if (output) { output->setPosition(0); output->truncate(); }
                    passed &= output != nullptr && PNGImageFormat().writeImageToStream(image, *output);
                    std::cout << "UI evidence: " << name << " " << editor->getWidth() << "x" << editor->getHeight() << '\n';
                }
            }
        }
        for (auto locale : {"es", "en"}) {
            AVDexedLanguageService::getInstance().setLanguage(locale);
            AVDexedLanguageService::getInstance().sendSynchronousChangeMessage();
            AVDexedSettingsPanel settings(processor, {});
            for (auto page : {AVDexedSettingsPanel::AVPage::audio, AVDexedSettingsPanel::AVPage::instrument,
                              AVDexedSettingsPanel::AVPage::midi, AVDexedSettingsPanel::AVPage::sysex,
                              AVDexedSettingsPanel::AVPage::about}) {
                settings.AVShowPage(page);
                const auto filename = String("Settings-") + String(int(page)) + "-" + locale + ".png";
                auto output = directory.getChildFile(filename).createOutputStream();
                auto img = settings.createComponentSnapshot(settings.getLocalBounds());
                if (output) { output->setPosition(0); output->truncate(); }
                passed &= output != nullptr && PNGImageFormat().writeImageToStream(img, *output);
            }
        }
        // Search/filter changes select rows, never load a sound.
        editor->playView.searchField.setText("Dexed_01");
        passed &= editor->playView.visibleEntries.size() >= 32;
        editor->playView.searchField.setText({});
        editor->playView.setSelectedFolder("favorites");
        editor->playView.setSelectedFolder("all");
        passed &= processor.keyboardState.isNoteOn(2, 60);
        MemoryBlock after;
        processor.getStateInformation(after);
        passed &= initial == after;
        // A bank-index request must commit the bank carried by that row/token.
        if (!editor->playView.visibleEntries.empty()) {
            const auto entry = editor->playView.visibleEntries.front();
            editor->playView.requestEntryLoad(0);
            if (entry.bank) {
                const auto expected = entry.bank->cartridge.unpackProgram(entry.program);
                passed &= std::memcmp(expected.getRawData(), processor.activeProgram.getRawData(), Program::PROGRAM_SIZE) == 0;
            }
            passed &= processor.loadAVStateInformation(initial.getData(), int(initial.getSize()));
        }
        editor.reset();
        // Editor recreation is a separate path from state restore / app restart.
        editor = std::make_unique<DexedAudioProcessorEditor>(&processor);
        processor.getStateInformation(after);
        passed &= initial == after && processor.getAVPresetName() == String::fromUTF8("Órbita oceánica – AV");
        editor.reset();
        // Validate all bundled SVGs in JUCE, not merely with a browser renderer.
        for (int i = 0; i < BinaryData::namedResourceListSize; ++i) {
            const String original(BinaryData::originalFilenames[i]);
            if (!original.endsWithIgnoreCase(".svg")) continue;
            int size = 0;
            const auto* data = BinaryData::getNamedResource(BinaryData::namedResourceList[i], size);
            auto xml = XmlDocument::parse(String::fromUTF8(data, size));
            passed &= xml != nullptr && Drawable::createFromSVG(*xml) != nullptr;
        }
        completed = true;
#if !JUCE_MAC
        MessageManager::getInstance()->stopDispatchLoop();
#endif
        });
#if JUCE_MAC
        // Console apps have no NSApp run loop. Pump JUCE's CF message source;
        // otherwise a deferred renderer could falsely report success without running.
        const auto deadline = Time::getMillisecondCounterHiRes() + 15000;
        while (!completed && Time::getMillisecondCounterHiRes() < deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
#else
        MessageManager::getInstance()->runDispatchLoop();
#endif
        passed &= completed;
    }
    DexedAudioProcessor::dexedAppDir = oldAppDirectory;
    DexedAudioProcessor::dexedCartDir = oldCartDirectory;
    AVDexedLanguageService::getInstance().configure(oldAppDirectory);
    AVDexedLanguageService::getInstance().setLanguage(oldLanguage);
    std::cout << (passed ? "UI geometry, state/view invariance and SVG checks passed.\n" : "UI QA failed.\n");
    return passed;
}
