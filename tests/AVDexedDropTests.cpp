// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// Files dropped on the editor, and files the system hands it (a double-click
// in the Finder, a drop on the Dock icon): .avdexpreset sounds alone and in
// folders, DX7 banks and mixes of them, through the real editor and its
// in-editor dialogs, in an isolated library under /tmp. The dialogs answer and
// the copies run on the message thread, which the test pumps between steps.
// That pumping is macOS-only (as the UI evidence): elsewhere only the
// synchronous checks run.

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "../source/ui/PluginEditor.h"
#include "../source/ui/AVDexedStandaloneAccess.h"
#include "../source/core/AVDexedLibrary.h"
#include "../source/core/Program.h"
#include "../source/parameter/Model.h"
#include <algorithm>
#include <functional>
#include <iostream>

namespace {
bool expect(bool condition, const juce::String& message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

#if JUCE_MAC
constexpr bool canPump = true;
/** Runs the message thread for a while: console apps have no NSApp run loop,
    so JUCE's CF message source is pumped directly (as the UI evidence does). */
void pump(int milliseconds)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + milliseconds;
    while (juce::Time::getMillisecondCounterHiRes() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
}
#else
constexpr bool canPump = false;
void pump(int) {}
#endif

/** Pumps until `condition` holds or `timeoutMs` have passed; says which. */
bool pumpUntil(const std::function<bool()>& condition, int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (! condition() && juce::Time::getMillisecondCounterHiRes() < deadline)
        pump(20);
    return condition();
}

juce::TextEditor* promptField(AVDexedPromptOverlay& prompt)
{
    for (auto* child : prompt.getChildren())
        if (auto* field = dynamic_cast<juce::TextEditor*>(child))
            return field;
    return nullptr;
}

/** The text in the dialog's field: the suggested folder name. */
juce::String promptText(AVDexedPromptOverlay& prompt)
{
    auto* field = promptField(prompt);
    return field != nullptr ? field->getText() : juce::String();
}

/** Sound files (.avdexpreset in any case) directly in `folder`. */
int presetsIn(const juce::File& folder)
{
    int count = 0;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findFiles))
        if (AVDexedLibrary::isPresetFile(entry.getFile()))
            ++count;
    return count;
}

/** Files called `name` anywhere below `root` (hidden folders included). */
int filesNamed(const juce::File& root, const juce::String& name)
{
    int count = 0;
    for (const auto& entry : juce::RangedDirectoryIterator(root, true, "*", juce::File::findFiles))
        if (entry.getFile().getFileName().equalsIgnoreCase(name))
            ++count;
    return count;
}

juce::MemoryBlock makeBank()
{
    Cartridge bank;
    auto program = Program::initialProgram();
    for (int slot = 0; slot < 32; ++slot)
        bank.packProgram(program.getRawData(), slot, "DROP " + juce::String(slot));
    return juce::MemoryBlock(bank.getVoiceSysex(), SYSEX_SIZE);
}
}

bool runAVDexedDropTests()
{
    using namespace juce;
    const auto oldAppDirectory = DexedAudioProcessor::dexedAppDir;
    const auto oldCartDirectory = DexedAudioProcessor::dexedCartDir;
    const auto oldLanguage = AVDexedLanguageService::getInstance().getLanguage();
    // An isolated data folder (the library) and a folder "outside" it, under /tmp.
    const auto home = File::getSpecialLocation(File::tempDirectory).getChildFile("avdexed-droptests-" + Uuid().toString());
    const auto library = home.getChildFile("AV Dexed");
    const auto outside = home.getChildFile("Downloads");
    library.createDirectory();
    outside.createDirectory();
    DexedAudioProcessor::dexedAppDir = library;
    DexedAudioProcessor::dexedCartDir = AVDexedLibrary::rootFor(library);
    const auto root = DexedAudioProcessor::dexedCartDir;
    AVDexedLanguageService::getInstance().configure(DexedAudioProcessor::getPreferencesFile());
    AVDexedLanguageService::getInstance().setLanguage("es");
    bool passed = expect(AVDexedLibrary::installFactorySounds(root, BinaryData::builtin_pgm_zip,
                                                              (size_t) BinaryData::builtin_pgm_zipSize, false) > 1,
                         "the factory sounds seed the isolated library");
    {
        DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
        const auto factoryBank = AVDexedLibrary::folderNamed(root, AVDexedLibrary::factoryFolderName).getChildFile("Dexed_01.syx");
        Cartridge bank;
        if (AVDexedLibrary::readBank(factoryBank, bank)) {
            processor.setActiveBank(bank, factoryBank);
            processor.setCurrentProgram(0);
        } else {
            passed = expect(false, "the factory bank is in the isolated library");
        }
        processor.setRateAndBufferSizeDetails(48000.0, 64);
        processor.prepareToPlay(48000.0, 64);
        processor.captureAVPresetBaseline();
        auto editor = std::make_unique<DexedAudioProcessorEditor>(&processor);

        // The dialog open in the editor, if any (a friend's lambda: the list is private).
        const auto openPrompt = [&editor]() -> AVDexedPromptOverlay* {
            for (auto& prompt : editor->AVPrompts)
                if (! prompt->isFinishing())
                    return prompt.get();
            return nullptr;
        };
        // Return or Escape in the dialog (after typing `text`, when given), then
        // waits for its result to have run (it arrives asynchronously; the copies
        // and the load happen in it) and for the dialog to be released after its
        // fade, which comes after the result.
        const auto answerPrompt = [&editor](AVDexedPromptOverlay& prompt, bool confirm, const String& text = {}) {
            if (text.isNotEmpty())
                if (auto* field = promptField(prompt))
                    field->setText(text, false);
            auto* answered = &prompt;
            prompt.keyPressed(KeyPress(confirm ? KeyPress::returnKey : KeyPress::escapeKey));
            pumpUntil([&editor, answered] {
                return ! editor->AVInPromptResult
                    && std::none_of(editor->AVPrompts.begin(), editor->AVPrompts.end(),
                                    [answered](const auto& open) { return open.get() == answered; });
            }, 5000);
            pump(50);
        };
        const auto writeSound = [&processor](const File& file, const String& name) {
            const auto state = processor.createAVNativePresetState(name, Uuid().toString());
            return state.getSize() > 0 && file.replaceWithData(state.getData(), state.getSize());
        };
        const auto sound = outside.getChildFile("LUISMI VIBES PIANO.avdexpreset");
        passed &= expect(writeSound(sound, "Luismi Piano"), "a sound file is written outside the library");
        const auto path = [](const File& file) { return file.getFullPathName(); };

        // What a drag is accepted: sounds, banks, folders and mixes of them; a
        // single tuning file; nothing else.
        passed &= expect(editor->isInterestedInFileDrag({ path(sound) }), "a single .avdexpreset is accepted");
        passed &= expect(editor->isInterestedInFileDrag({ path(outside.getChildFile("X.AVDEXPRESET")) }), "the extension counts in any case");
        passed &= expect(editor->isInterestedInFileDrag({ path(outside.getChildFile("Old.dexedpreset")) }), "the old .dexedpreset name is accepted");
        passed &= expect(editor->isInterestedInFileDrag({ path(outside) }), "a folder is accepted");
        passed &= expect(editor->isInterestedInFileDrag({ path(sound), path(outside.getChildFile("Bank.syx")), path(outside) }),
                         "sounds, banks and folders mixed are accepted");
        passed &= expect(! editor->isInterestedInFileDrag({ path(outside.getChildFile("notes.txt")) }), "another kind of file is refused");
        passed &= expect(! editor->isInterestedInFileDrag({ path(sound), path(outside.getChildFile("notes.txt")) }),
                         "a sound with another kind of file is refused");
        passed &= expect(editor->isInterestedInFileDrag({ path(outside.getChildFile("scale.scl")) })
                             && ! editor->isInterestedInFileDrag({ path(outside.getChildFile("a.scl")), path(outside.getChildFile("b.kbm")) }),
                         "one tuning file loads on its own, two do not");
        passed &= expect(! editor->isInterestedInFileDrag({}), "an empty drag is refused");

        // The command line the system hands the app (quoted paths, flags).
        const auto paths = AVDexedStandaloneAccess::AVSystemOpenPaths(
            path(sound).quoted() + " -psn_0_123 " + path(outside).quoted() + " "
            + path(outside.getChildFile("missing.avdexpreset")).quoted() + " " + path(sound).quoted());
        passed &= expect(paths.size() == 2 && paths[0] == path(sound) && paths[1] == path(outside),
                         "the command line gives its existing paths once, unquoted, flags left out");
        passed &= expect(AVDexedStandaloneAccess::AVSystemOpenPaths({}).isEmpty()
                             && AVDexedStandaloneAccess::AVSystemOpenPaths("-NSDocumentRevisionsDebugMode YES").isEmpty(),
                         "a command line without files gives nothing");
        // A quote inside a name arrives escaped (\"), with and without spaces around it.
        const auto quotedName = outside.getChildFile("Mi \"Piano\" favorito.avdexpreset");
        const auto quotedNoSpace = outside.getChildFile("12\".avdexpreset");
        passed &= expect(writeSound(quotedName, "Comillas") && writeSound(quotedNoSpace, "Pulgadas"),
                         "sounds with a quote in their name are written outside");
        const auto escaped = [](const File& file) { return file.getFullPathName().replace("\"", "\\\""); };
        const auto quotedPaths = AVDexedStandaloneAccess::AVSystemOpenPaths(
            "\"" + escaped(quotedName) + "\" " + escaped(quotedNoSpace) + " " + path(sound).quoted());
        passed &= expect(quotedPaths.size() == 3 && quotedPaths[0] == path(quotedName) && quotedPaths[1] == path(quotedNoSpace)
                             && quotedPaths[2] == path(sound),
                         "names with a quote inside arrive as the system escapes them");

        if (! canPump) {
            std::cout << "Drop checks: the dialogs and copies run on macOS only.\n";
        } else {
            passed &= expect(pumpUntil([&] { return ! editor->playView.libraryIndexing; }, 15000), "the library is indexed");
            const auto userPresets = root.getChildFile(AVDexedLibrary::userPresetsFolderName);

            // A sound from outside: the folder question (User Presets suggested),
            // then the copy, the load and the footer line.
            editor->filesDropped({ path(sound) }, 10, 10);
            auto* prompt = openPrompt();
            passed &= expect(prompt != nullptr && promptText(*prompt) == AVDexedLibrary::userPresetsFolderName,
                             "a sound from outside asks for a folder, User Presets suggested");
            passed &= expect(! editor->isInterestedInFileDrag({ path(sound) }), "no drop while the dialog is open");
            if (prompt != nullptr) answerPrompt(*prompt, true);
            const auto copied = userPresets.getChildFile(sound.getFileName());
            passed &= expect(copied.existsAsFile() && copied.getSize() == sound.getSize(), "the sound is copied into User Presets");
            passed &= expect(pumpUntil([&] { return processor.getAVPresetName() == "Luismi Piano"; }, 3000), "the imported sound loads");
            passed &= expect(editor->AVNoticeText.contains(AVDexedLibrary::userPresetsFolderName), "the footer tells where it went");
            passed &= expect(openPrompt() == nullptr, "no further dialog after the import");

            // The same file again: asked again, nothing copied twice.
            editor->AVNoticeText.clear();
            editor->filesDropped({ path(sound) }, 10, 10);
            prompt = openPrompt();
            passed &= expect(prompt != nullptr, "the same file from outside asks again");
            if (prompt != nullptr) answerPrompt(*prompt, true, AVDexedLibrary::userPresetsFolderName);
            passed &= expect(presetsIn(userPresets) == 1 && filesNamed(root, "LUISMI VIBES PIANO 2.avdexpreset") == 0,
                             "the same content is not copied twice");
            const auto alreadyInUserPresets = AVDexedLanguageService::getInstance().format("alreadyInFolder",
                                                                                           {{"folder", AVDexedLibrary::userPresetsFolderName}});
            passed &= expect(editor->AVNoticeText == alreadyInUserPresets, "the footer says it was already there");

            // A folder of sounds (a subfolder, system leftovers and a stray file
            // inside): its name is suggested, every sound is copied, nothing else.
            const auto dropped = outside.getChildFile("Mis sonidos");
            dropped.getChildFile(String::fromUTF8("Más")).createDirectory();
            passed &= expect(writeSound(dropped.getChildFile("Uno.avdexpreset"), "Uno")
                                 && writeSound(dropped.getChildFile("Dos.avdexpreset"), "Dos")
                                 && writeSound(dropped.getChildFile("Tres.AVDEXPRESET"), "Tres")
                                 && writeSound(dropped.getChildFile(String::fromUTF8("Más/Cuatro.avdexpreset")), "Cuatro")
                                 && dropped.getChildFile(".DS_Store").replaceWithText("junk")
                                 && dropped.getChildFile("notas.txt").replaceWithText("junk"),
                             "a folder of sounds is written outside the library");
            editor->filesDropped({ path(dropped) }, 10, 10);
            prompt = openPrompt();
            passed &= expect(prompt != nullptr && promptText(*prompt) == "Mis sonidos", "a folder of sounds suggests its own name");
            if (prompt != nullptr) answerPrompt(*prompt, true);
            const auto imported = root.getChildFile("Mis sonidos");
            passed &= expect(presetsIn(imported) == 4 && imported.getChildFile("Cuatro.avdexpreset").existsAsFile()
                                 && imported.getChildFile("Tres.avdexpreset").existsAsFile(),
                             "every sound of the folder, subfolders included, is copied under the current name");
            passed &= expect(! imported.getChildFile("notas.txt").exists() && ! imported.getChildFile(".DS_Store").exists()
                                 && ! imported.getChildFile(String::fromUTF8("Más")).exists(),
                             "nothing else of the folder is copied");
            passed &= expect(pumpUntil([&] { return processor.getAVPresetName() == "Dos"; }, 3000), "the first sound of the folder loads");
            passed &= expect(editor->AVNoticeText.contains("Mis sonidos") && editor->AVNoticeText.contains("4"),
                             "the footer names the folder and counts the 4 sounds");

            // A sound already in the library: no question, it loads, the footer says so.
            editor->AVNoticeText.clear();
            editor->filesDropped({ path(copied) }, 10, 10);
            passed &= expect(openPrompt() == nullptr, "a sound already in the library asks nothing");
            passed &= expect(pumpUntil([&] { return processor.getAVPresetName() == "Luismi Piano"; }, 3000), "it just loads");
            passed &= expect(editor->AVNoticeText == alreadyInUserPresets, "and the footer says it was already in its folder");
            passed &= expect(presetsIn(userPresets) == 1, "nothing is copied");

            // Several sounds already in one library folder, then from different folders.
            editor->AVNoticeText.clear();
            editor->filesDropped({ path(imported.getChildFile("Uno.avdexpreset")), path(imported.getChildFile("Dos.avdexpreset")) }, 10, 10);
            passed &= expect(openPrompt() == nullptr
                                 && editor->AVNoticeText == AVDexedLanguageService::getInstance().format("alreadyInFolderMany", {{"folder", "Mis sonidos"}}),
                             "several sounds of one library folder get the plural line with the folder");
            pump(100);
            editor->AVNoticeText.clear();
            editor->filesDropped({ path(copied), path(imported.getChildFile("Uno.avdexpreset")) }, 10, 10);
            passed &= expect(openPrompt() == nullptr
                                 && editor->AVNoticeText == AVDexedLanguageService::getInstance().text("alreadyInLibraryMany"),
                             "sounds from several library folders get the general line");
            pump(100);

            // An old .dexedpreset from outside arrives under the current name.
            const auto legacy = outside.getChildFile("Viejo.dexedpreset");
            passed &= expect(writeSound(legacy, "Viejo"), "an old-named sound is written outside");
            editor->filesDropped({ path(legacy) }, 10, 10);
            prompt = openPrompt();
            if (prompt != nullptr) answerPrompt(*prompt, true, AVDexedLibrary::userPresetsFolderName);
            passed &= expect(userPresets.getChildFile("Viejo.avdexpreset").existsAsFile() && ! userPresets.getChildFile("Viejo.dexedpreset").exists(),
                             "an old .dexedpreset arrives as .avdexpreset");

            // A bank and a sound together: the bank's name is suggested, both
            // land there, the bank loads first, 33 sounds are counted.
            const auto bankFile = outside.getChildFile("Finetales.syx");
            const auto bankBytes = makeBank();
            passed &= expect(bankFile.replaceWithData(bankBytes.getData(), bankBytes.getSize()), "a bank is written outside");
            const auto extra = outside.getChildFile("Extra.avdexpreset");
            passed &= expect(writeSound(extra, "Extra"), "another sound is written outside");
            editor->filesDropped({ path(extra), path(bankFile) }, 10, 10);
            prompt = openPrompt();
            passed &= expect(prompt != nullptr && promptText(*prompt) == "Finetales", "a bank with sounds suggests the bank's name");
            if (prompt != nullptr) answerPrompt(*prompt, true);
            const auto bankFolder = root.getChildFile("Finetales");
            passed &= expect(bankFolder.getChildFile("Finetales.syx").existsAsFile() && bankFolder.getChildFile("Extra.avdexpreset").existsAsFile(),
                             "the bank and the sound land in that folder");
            passed &= expect(pumpUntil([&] { return processor.activeFileCartridge == bankFolder.getChildFile("Finetales.syx"); }, 3000),
                             "the bank loads first");
            passed &= expect(editor->AVNoticeText.contains("33"), "32 voices and 1 sound are counted");

            // Escape in the folder question: nothing is copied.
            const auto cancelled = outside.getChildFile("Cancelado.avdexpreset");
            passed &= expect(writeSound(cancelled, "Cancelado"), "a sound to cancel is written outside");
            editor->filesDropped({ path(cancelled) }, 10, 10);
            prompt = openPrompt();
            passed &= expect(prompt != nullptr, "the cancelled sound asked for a folder");
            if (prompt != nullptr) answerPrompt(*prompt, false);
            passed &= expect(filesNamed(root, "Cancelado.avdexpreset") == 0, "Escape copies nothing");

            // Files the system hands the app (Finder, Dock) wait for an open
            // dialog, then are imported like a drop; other kinds are ignored.
            editor->AVOpenFilesFromSystem({ path(outside.getChildFile("notes.txt")), path(outside.getChildFile("missing.avdexpreset")) });
            pump(100);
            passed &= expect(openPrompt() == nullptr, "files the app does not open ask nothing");
            const auto fromFinder = outside.getChildFile("Desde Finder.avdexpreset");
            passed &= expect(writeSound(fromFinder, "Desde Finder"), "a sound to open from the Finder is written outside");
            editor->filesDropped({ path(cancelled) }, 10, 10);
            prompt = openPrompt();
            editor->AVOpenFilesFromSystem({ path(fromFinder) });
            pump(100);
            passed &= expect(prompt != nullptr && openPrompt() == prompt, "a file from the system waits for the open dialog");
            if (prompt != nullptr) answerPrompt(*prompt, false);
            passed &= expect(pumpUntil([&] { return openPrompt() != nullptr; }, 2000), "then it asks for its folder");
            prompt = openPrompt();
            if (prompt != nullptr) answerPrompt(*prompt, true, AVDexedLibrary::userPresetsFolderName);
            passed &= expect(userPresets.getChildFile("Desde Finder.avdexpreset").existsAsFile(), "and is imported like a drop");
            passed &= expect(pumpUntil([&] { return processor.getAVPresetName() == "Desde Finder"; }, 3000), "and loads");

            // With the sound being edited: the import still copies; loading the
            // new sound waits for the save-changes question, and Cancel keeps
            // the edit.
            if (auto* cutoff = dynamic_cast<RangedAudioParameter*>(processor.parameters.getParameter(IDs::cutoff.name)))
                cutoff->setValueNotifyingHost(cutoff->getValue() > 0.5f ? 0.1f : 0.9f);
            editor->playView.refreshLabels();
            passed &= expect(editor->playView.isModified(), "a tweak marks the sound as modified");
            const auto tweaked = outside.getChildFile("Durante.avdexpreset");
            passed &= expect(writeSound(tweaked, "Durante"), "a sound to import while editing is written outside");
            editor->filesDropped({ path(tweaked) }, 10, 10);
            prompt = openPrompt();
            if (prompt != nullptr) answerPrompt(*prompt, true, AVDexedLibrary::userPresetsFolderName);
            passed &= expect(userPresets.getChildFile("Durante.avdexpreset").existsAsFile(), "the sound is copied before the save-changes question");
            prompt = openPrompt();
            passed &= expect(prompt != nullptr && processor.getAVPresetName() != "Durante", "loading it waits for the save-changes question");
            if (prompt != nullptr) answerPrompt(*prompt, false);
            passed &= expect(processor.getAVPresetName() != "Durante" && editor->playView.isModified(), "Cancel keeps the sound being edited");
            passed &= expect(openPrompt() == nullptr, "no dialog is left open");
        }
        editor.reset();
        pump(300);
    }
    DexedAudioProcessor::dexedAppDir = oldAppDirectory;
    DexedAudioProcessor::dexedCartDir = oldCartDirectory;
    if (home.isAChildOf(File::getSpecialLocation(File::tempDirectory)) && home.getFileName().startsWith("avdexed-droptests-"))
        home.deleteRecursively();
    AVDexedLanguageService::getInstance().configure(oldAppDirectory == File() ? File()
                                                    : oldAppDirectory.getChildFile(DexedAudioProcessor::preferencesFileName));
    AVDexedLanguageService::getInstance().setLanguage(oldLanguage);
    std::cout << (passed ? "Drop and open-with checks passed.\n" : "Drop and open-with checks FAILED.\n");
    return passed;
}
