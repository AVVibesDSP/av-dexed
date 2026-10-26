// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// Files dropped on the editor, and files the system hands it (a double-click
// in the Finder, a drop on the Dock icon): .avdexpreset sounds alone and in
// folders, DX7 banks and mixes of them, through the real editor and its
// in-editor dialogs, in an isolated library under /tmp. The dialogs answer and
// the copies run on the message thread, which the test pumps between steps.
// That pumping is macOS and Windows only: elsewhere only the synchronous
// checks run.

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
#elif JUCE_WINDOWS
}
namespace juce::detail {
// JUCE's own step of runDispatchLoopUntil (juce_Messaging_windows.cpp), which
// JUCE_MODAL_LOOPS_PERMITTED=0 leaves out.
bool dispatchNextMessageOnSystemQueue(bool returnIfNoPendingMessages);
}
namespace {
constexpr bool canPump = true;
/** Runs the message thread for a while: a console app has no message loop, so
    the thread's Windows messages (JUCE's queue, its timers) are dispatched here. */
void pump(int milliseconds)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + milliseconds;
    while (juce::Time::getMillisecondCounterHiRes() < deadline)
        if (! juce::detail::dispatchNextMessageOnSystemQueue(true))
            juce::Thread::sleep(1);
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

juce::MemoryBlock makeBank(int seed = 0)
{
    // 32 different sounds (an Init voice would be an empty slot): the Init
    // voice with each slot's own algorithm.
    Cartridge bank;
    for (int slot = 0; slot < 32; ++slot) {
        uint8_t data[Program::PROGRAM_SIZE + 1];
        std::memcpy(data, Program::initialProgram().getRawData(), sizeof(data));
        data[134] = uint8_t(slot);
        data[144] = uint8_t(12 + seed % 25);   // another seed, other sounds
        bank.packProgram(data, slot, "DROP " + juce::String(slot));
    }
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
        // A relative path (a launch from a terminal, ".\sonido.avdexpreset" on
        // Windows) is taken from the folder the app was started in.
        const auto separator = File::getSeparatorString();
        const auto inOutside = outside.getFileName() + separator + sound.getFileName();
        const auto relativePaths = AVDexedStandaloneAccess::AVSystemOpenPaths(
            inOutside.quoted() + " " + ("." + separator + inOutside).quoted() + " missing.avdexpreset -psn_0_123 "
                + outside.getFileName(),
            outside.getParentDirectory());
        passed &= expect(relativePaths.size() == 2 && relativePaths[0] == path(sound) && relativePaths[1] == path(outside),
                         "relative paths are taken from the starting folder, once each, as full paths");
        passed &= expect(AVDexedStandaloneAccess::AVSystemOpenPaths(sound.getFileName().quoted(), outside.getParentDirectory()).isEmpty()
                             && AVDexedStandaloneAccess::AVSystemOpenPaths("-NSDocumentRevisionsDebugMode YES", outside).isEmpty(),
                         "a relative name that is not there, or a flag, gives nothing");
#if ! JUCE_WINDOWS
        // A quote inside a name arrives escaped (\"), with and without spaces
        // around it. Windows allows no quote in a file name.
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
#endif

        if (! canPump) {
            std::cout << "Drop checks: the dialogs and copies run on macOS and Windows only.\n";
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

            // A bank and a sound together: the bank's name is suggested, the
            // bank becomes 32 sounds there (its file kept in the backups), the
            // first of them loads, 33 sounds are counted.
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
            passed &= expect(pumpUntil([&] { return processor.getAVPresetPath() == bankFolder.getChildFile("DROP 0.avdexpreset"); }, 5000),
                             "the first sound of the bank loads");
            passed &= expect(presetsIn(bankFolder) == 33 && ! bankFolder.getChildFile("Finetales.syx").exists()
                                 && bankFolder.getChildFile("Extra.avdexpreset").existsAsFile(),
                             "the bank becomes 32 sounds in that folder, next to the sound");
            passed &= expect(editor->AVNoticeText.contains("33"), "32 voices and 1 sound are counted");
            bool backedUp = false;
            for (const auto& entry : RangedDirectoryIterator(root.getChildFile(AVDexedLibrary::syxBackupsFolderName), true, "Finetales.syx", File::findFiles))
                backedUp |= entry.getFile().hasIdenticalContentTo(bankFile);
            passed &= expect(backedUp && bankFile.existsAsFile(), "the bank's file is kept in the backups, and the dropped one stays");
            pump(200);

            // Several banks at once: one question, a folder per bank inside the
            // folder chosen, which the browser shows (all of them, one list).
            const auto several = outside.getChildFile("Varios");
            several.createDirectory();
            StringArray severalPaths;
            for (int i = 0; i < 3; ++i) {
                const auto file = several.getChildFile("Banco " + String(i + 1) + ".syx");
                const auto bytes = makeBank(i + 1);
                file.replaceWithData(bytes.getData(), bytes.getSize());
                severalPaths.add(path(file));
            }
            editor->filesDropped(severalPaths, 10, 10);
            prompt = openPrompt();
            passed &= expect(prompt != nullptr, "several banks ask for one folder");
            if (prompt != nullptr) answerPrompt(*prompt, true, "Mis bancos");
            const auto mine = root.getChildFile("Mis bancos");
            passed &= expect(pumpUntil([&] { return presetsIn(mine.getChildFile("Banco 3")) == 32; }, 5000)
                                 && presetsIn(mine.getChildFile("Banco 1")) == 32 && presetsIn(mine) == 0,
                             "several banks become a folder each inside the folder chosen");
            passed &= expect(pumpUntil([&] { return editor->playView.selectedFolderKey == "folder:Mis bancos"; }, 3000),
                             "and the browser shows that folder");
            pump(200);

            // A dropped folder with folders in it: the same structure, a folder per bank.
            const auto collection = outside.getChildFile("Coleccion");
            for (const auto* name : { "Pianos/P1.syx", "Bajos/B1.syx" }) {
                const auto file = collection.getChildFile(name);
                file.getParentDirectory().createDirectory();
                const auto bytes = makeBank(7);
                file.replaceWithData(bytes.getData(), bytes.getSize());
            }
            editor->filesDropped({ path(collection) }, 10, 10);
            prompt = openPrompt();
            passed &= expect(prompt != nullptr && promptText(*prompt) == "Coleccion", "a dropped folder suggests its name");
            if (prompt != nullptr) answerPrompt(*prompt, true);
            passed &= expect(pumpUntil([&] { return presetsIn(root.getChildFile("Coleccion/Bajos/B1")) == 32; }, 5000)
                                 && presetsIn(root.getChildFile("Coleccion/Pianos/P1")) == 32,
                             "a dropped folder keeps its structure, a folder per bank");
            pump(200);

            // A long conversion shows its progress, and cancelling it leaves
            // the library and the backups as they were.
            const auto big = outside.getChildFile("Grande");
            big.createDirectory();
            for (int i = 0; i < 60; ++i) {
                const auto bytes = makeBank(i);
                big.getChildFile("G" + String(i + 1) + ".syx").replaceWithData(bytes.getData(), bytes.getSize());
            }
            const auto backupsBefore = root.getChildFile(AVDexedLibrary::syxBackupsFolderName).getNumberOfChildFiles(File::findDirectories);
            editor->filesDropped({ path(big) }, 10, 10);
            prompt = openPrompt();
            if (prompt != nullptr) answerPrompt(*prompt, true);
            AVDexedPromptOverlay* progress = nullptr;
            passed &= expect(pumpUntil([&] { progress = editor->AVConversionPrompt.getComponent(); return progress != nullptr; }, 4000)
                                 && processor.isAVConverting(),
                             "a long conversion shows its progress");
            if (progress != nullptr) progress->keyPressed(KeyPress(KeyPress::escapeKey));
            passed &= expect(pumpUntil([&] { return ! processor.isAVConverting() && openPrompt() != nullptr; }, 8000),
                             "Escape cancels it, and a notice says so");
            if (auto* notice = openPrompt()) answerPrompt(*notice, true);
            passed &= expect(! root.getChildFile("Grande").exists()
                                 && root.getChildFile(AVDexedLibrary::syxBackupsFolderName).getNumberOfChildFiles(File::findDirectories) == backupsBefore,
                             "a cancelled import leaves the library and the backups as they were");
            pump(200);

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

            // A TX802 performance file (no banks in it) becomes a layers
            // preset per performance, its voices from the bank next to it.
            // The edited sound is asked about before the first one plays.
            {
                std::vector<uint8_t> pmem(84, 0);
                for (int tg = 0; tg < 8; ++tg) {
                    pmem[size_t(tg)] = uint8_t((tg << 5) | 16);       // every TG its own instrument, omni
                    pmem[size_t(8 + tg)] = uint8_t(tg);               // internal voices 1-8
                    pmem[size_t(24 + tg)] = 99;
                    pmem[size_t(32 + tg)] = uint8_t((7 << 3) | 3);    // centre detune, outputs I+II
                    pmem[size_t(48 + tg)] = 127;
                    pmem[size_t(56 + tg)] = 24;
                }
                const char* name = "Drop Performance    ";
                for (int i = 0; i < 20; ++i) pmem[size_t(64 + i)] = uint8_t(name[i]);
                std::vector<uint8_t> body;
                for (const char* c = "LM  8952PM"; *c; ++c) body.push_back(uint8_t(*c));
                for (const auto byte : pmem) {
                    body.push_back(uint8_t("0123456789ABCDEF"[byte >> 4]));
                    body.push_back(uint8_t("0123456789ABCDEF"[byte & 15]));
                }
                MemoryBlock dump;
                const uint8_t head[] = { 0xF0, 0x43, 0x00, 0x7E, 0x01, 0x28 };
                dump.append(head, sizeof(head));
                dump.append(body.data(), body.size());
                const uint8_t tail[] = { AVDexedSysex::checksum(body.data(), body.size()), 0xF7 };
                dump.append(tail, 2);
                const auto setFolder = outside.getChildFile("TX Set");
                setFolder.createDirectory();
                const auto performanceFile = setFolder.getChildFile("SetPerf.syx");
                performanceFile.replaceWithData(dump.getData(), dump.getSize());
                Cartridge voices;
                const auto init = Program::initialProgram();
                for (int slot = 0; slot < 32; ++slot) voices.packProgram(init.getRawData(), slot, "TXV " + String(slot + 1));
                setFolder.getChildFile("SetVoice32.syx").replaceWithData(voices.getVoiceSysex(), SYSEX_SIZE);
                const auto libraryFiles = AVDexedLibrary::findSoundFiles(root).size();

                passed &= expect(editor->isInterestedInFileDrag({ path(performanceFile) }), "a performance file is accepted");
                editor->filesDropped({ path(performanceFile) }, 10, 10);
                prompt = openPrompt();
                passed &= expect(prompt != nullptr && promptText(*prompt) == "SetPerf", "a performance file asks for its folder, as a bank does");
                if (prompt != nullptr) answerPrompt(*prompt, true, "Performances TX");
                const auto performancePreset = root.getChildFile("Performances TX").getChildFile("Drop Performance.avdexpreset");
                passed &= expect(pumpUntil([&] { return openPrompt() != nullptr; }, 5000) && performancePreset.existsAsFile(),
                                 "each performance becomes a layers preset; the edited sound is asked about before it plays");
                if (auto* question = openPrompt()) answerPrompt(*question, false);
                passed &= expect(! processor.isAVLayerModeOn(), "Cancel keeps the sound and leaves the layers alone");
                MemoryBlock performanceBytes;
                const auto performanceXml = performancePreset.loadFileAsData(performanceBytes)
                    ? AudioProcessor::getXmlFromBinary(performanceBytes.getData(), int(performanceBytes.getSize())) : nullptr;
                passed &= expect(performanceXml != nullptr && performanceXml->getChildByName("avLayers") != nullptr,
                                 "the layers preset carries the performance's layers");
                processor.captureAVPresetBaseline();   // nothing to save now
                editor->playView.refreshLabels();
                editor->loadNativePreset(performancePreset);
                pump(100);
                passed &= expect(openPrompt() == nullptr && processor.isAVLayerModeOn()
                                     && processor.getAVLayerProgram(7).getName().trim() == "TXV 8",
                                 "it plays its eight layers, the voices from the bank next to the performance file");
                passed &= expect(AVDexedLibrary::findSoundFiles(root).size() == libraryFiles + 1,
                                 "only the layers preset is added (the performance file goes to the backups)");

                // An old layers file (.avdexlayers, 0.3.2): the layers in an XML of their own.
                const auto layersFile = outside.getChildFile("Mis capas.avdexlayers");
                processor.getAVLayerSettings(2).transpose = 5;
                const auto layered = processor.createAVNativePresetState("Mis capas", Uuid().toString(), true);
                const auto layeredXml = AudioProcessor::getXmlFromBinary(layered.getData(), int(layered.getSize()));
                XmlElement legacy("avDexedLayers");
                legacy.setAttribute("formatVersion", 1);
                if (layeredXml != nullptr && layeredXml->getChildByName("avLayers") != nullptr)
                    legacy.addChildElement(new XmlElement(*layeredXml->getChildByName("avLayers")));
                layersFile.replaceWithText(legacy.toString());
                processor.getAVLayerSettings(2).transpose = 0;
                passed &= expect(editor->isInterestedInFileDrag({ path(layersFile) }), "a layers file is accepted");
                editor->filesDropped({ path(layersFile) }, 10, 10);
                pump(100);
                passed &= expect(processor.getAVLayerSettings(2).transpose.load() == 5 && editor->AVNoticeText.contains("Mis capas"),
                                 "a dropped layers file plays its layers");
                passed &= expect(openPrompt() == nullptr, "no dialog is left open after the layers");

                // "Guardar capas › Guardar como": the layers as a sound file
                // (.avdexpreset) in "User Presets", asked only for a name.
                const int presetsBefore = presetsIn(userPresets);
                editor->layersView.onSaveLayers(true, "Mis capas");
                prompt = openPrompt();
                passed &= expect(prompt != nullptr && promptText(*prompt) == "Mis capas", "saving the layers asks only for a name");
                if (prompt != nullptr) answerPrompt(*prompt, true, "Capas guardadas");
                const auto layersPreset = userPresets.getChildFile("Capas guardadas.avdexpreset");
                MemoryBlock savedLayers;
                const auto savedXml = layersPreset.loadFileAsData(savedLayers)
                    ? AudioProcessor::getXmlFromBinary(savedLayers.getData(), int(savedLayers.getSize())) : nullptr;
                passed &= expect(savedXml != nullptr && savedXml->getChildByName("avLayers") != nullptr
                                     && presetsIn(userPresets) == presetsBefore + 1 && processor.getAVLayerSetFile() == layersPreset,
                                 "the layers go into \"User Presets\" as an .avdexpreset that carries them");
                // "Guardar" writes the same file again, asking nothing.
                processor.getAVLayerSettings(2).transpose = 7;
                processor.selectAVLayer(4);   // another layer in Play: still the same layers preset
                editor->layersView.onSaveLayers(false, {});
                pump(50);
                passed &= expect(openPrompt() == nullptr && presetsIn(userPresets) == presetsBefore + 1
                                     && processor.getAVPresetPath() == layersPreset,
                                 "\"Guardar\" of the layers asks nothing and writes no other file");
                // Opened as any sound, it plays all its layers and keeps its name.
                processor.getAVLayerSettings(2).transpose = 0;
                processor.setAVLayerMode(false);
                editor->loadNativePreset(layersPreset);
                pump(100);
                if (auto* question = openPrompt()) answerPrompt(*question, false);   // not expected
                passed &= expect(processor.isAVLayerModeOn() && processor.getAVLayerSettings(2).transpose.load() == 7
                                     && processor.getAVSelectedLayer() == 4 && processor.getAVPresetName() == "Capas guardadas"
                                     && processor.getAVLayerSetFile() == layersPreset,
                                 "an opened layers preset plays its layers and keeps its name");
                // "Guardar" in Play keeps it a layers preset.
                processor.getAVLayerSettings(2).transpose = 3;
                editor->AVSaveSound();
                pump(50);
                MemoryBlock resaved;
                const auto resavedXml = layersPreset.loadFileAsData(resaved)
                    ? AudioProcessor::getXmlFromBinary(resaved.getData(), int(resaved.getSize())) : nullptr;
                const auto* resavedLayers = resavedXml != nullptr ? resavedXml->getChildByName("avLayers") : nullptr;
                passed &= expect(resavedLayers != nullptr && resavedLayers->getChildElement(2) != nullptr
                                     && resavedLayers->getChildElement(2)->getIntAttribute("transpose") == 3,
                                 "\"Guardar\" in Play writes a layers preset with its layers");

                // A sound without layers opened from Play plays alone (the
                // layers stay, off); on the Capas page it goes into the
                // selected layer.
                const auto single = userPresets.getChildFile("Solo.avdexpreset");
                passed &= expect(writeSound(single, "Solo"), "a single sound is written into the library");
                processor.captureAVPresetBaseline();
                editor->playView.refreshLabels();
                editor->showAVView(DexedAudioProcessorEditor::AVView::layers, false);
                const int selectedLayer = processor.getAVSelectedLayer();
                editor->loadNativePreset(single);
                pump(100);
                passed &= expect(processor.isAVLayerModeOn() && processor.getAVSelectedLayer() == selectedLayer
                                     && processor.getAVLayerSettings(2).transpose.load() == 3
                                     && processor.getAVPresetName() == "Solo",
                                 "on the Capas page a single sound goes into the selected layer");
                editor->showAVView(DexedAudioProcessorEditor::AVView::play, false);
                processor.captureAVPresetBaseline();
                editor->playView.refreshLabels();
                editor->loadNativePreset(single);
                pump(100);
                passed &= expect(! processor.isAVLayerModeOn() && processor.getAVPresetName() == "Solo"
                                     && processor.getAVLayerSettings(2).transpose.load() == 3,
                                 "from Play a single sound plays alone, the layers kept for the Capas page");
                editor->loadNativePreset(layersPreset);
                pump(100);
                passed &= expect(processor.isAVLayerModeOn(), "a layers preset turns the layers on again");

                // A sound saved into a folder the browser does not show: its folder opens.
                editor->playView.setSelectedFolder("folder:" + String(AVDexedLibrary::factoryFolderName));
                editor->layersView.onSaveLayers(true, {});
                prompt = openPrompt();
                if (prompt != nullptr) answerPrompt(*prompt, true, "Visible");
                passed &= expect(pumpUntil([&editor] {
                                     return editor->playView.selectedFolderKey == "folder:" + String(AVDexedLibrary::userPresetsFolderName);
                                 }, 3000),
                                 "after saving, the browser opens the folder that holds the new preset");
                processor.setAVLayerMode(false);
            }
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
