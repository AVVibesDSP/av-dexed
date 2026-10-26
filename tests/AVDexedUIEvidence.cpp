// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "../source/ui/PluginEditor.h"
#include "../source/core/AVDexedLibrary.h"
#include "../source/ui/window/AVDexedSettingsPanel.h"
#include "../source/ui/window/MidiCCListenerStatus.h"
#include "../source/ui/window/TuningShow.h"
#include "../source/parameter/Model.h"
#include <cmath>
#include <functional>
#include <iostream>

// Visual QA of the real editor. These PNGs are
// evidence only: no raster data is loaded or embedded in the instrument UI.
bool AVRenderUIEvidence(const juce::File& directory) {
    using namespace juce;
    directory.createDirectory();
    const auto oldAppDirectory = DexedAudioProcessor::dexedAppDir;
    const auto oldCartDirectory = DexedAudioProcessor::dexedCartDir;
    const auto oldLanguage = AVDexedLanguageService::getInstance().getLanguage();
    // The data folder is the library, so the isolated one is a fresh temporary
    // root seeded with the factory sounds, the way a first install is: the Play
    // view indexes it exactly as it would the user's folder (never touched here).
    const auto libraryRoot = File::getSpecialLocation(File::tempDirectory)
                                 .getChildFile("avdexed-uievidence-" + Uuid().toString());
    libraryRoot.createDirectory();
    DexedAudioProcessor::dexedAppDir = libraryRoot;
    DexedAudioProcessor::dexedCartDir = AVDexedLibrary::rootFor(libraryRoot);
    bool passed = AVDexedLibrary::installFactorySounds(DexedAudioProcessor::dexedCartDir, BinaryData::builtin_pgm_zip,
                                                       (size_t) BinaryData::builtin_pgm_zipSize, false) > 1;
    if (!passed) std::cerr << "Could not seed the isolated library with the factory sounds.\n";
    bool completed = false;
    {
        DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
        // As at startup: Dexed_01 from the factory folder is the active bank.
        const auto factoryBank = AVDexedLibrary::folderNamed(DexedAudioProcessor::dexedCartDir, AVDexedLibrary::factoryFolderName)
                                     .getChildFile("Dexed_01.syx");
        Cartridge bank;
        if (AVDexedLibrary::readBank(factoryBank, bank)) {
            processor.setActiveBank(bank, factoryBank);
            processor.setCurrentProgram(4);
        } else {
            std::cerr << "The factory bank is missing from the isolated library.\n";
            passed = false;
        }
        processor.setRateAndBufferSizeDetails(48000.0, 64);
        processor.prepareToPlay(48000.0, 64);
        processor.setAVPresetContext(String::fromUTF8("Órbita oceánica – AV"), "visual-evidence", {});
        processor.captureAVPresetBaseline();
        MemoryBlock initial;
        processor.getStateInformation(initial);
        auto editor = std::make_unique<DexedAudioProcessorEditor>(&processor);
        processor.keyboardState.noteOn(2, 60, 0.8f); // unrelated incoming MIDI must survive UI changes
        // The library is read on a background thread: give it time, then poll
        // (bounded) so a slow disk delays the renders instead of failing them.
        const auto indexDeadline = Time::getMillisecondCounterHiRes() + 12000;
        std::function<void()> render;
        render = [&] {
        if (editor->playView.libraryIndexing && Time::getMillisecondCounterHiRes() < indexDeadline) {
            Timer::callAfterDelay(100, render);
            return;
        }
        passed &= !editor->playView.libraryIndexing && editor->playView.libraryEntries.size() > 32;
        std::cout << "Indexed presets: " << editor->playView.libraryEntries.size() << '\n';
        // What plays now, to tell whether a key or a pedal changed the sound.
        const auto currentSound = [&processor] {
            return processor.getAVPresetName() + "|" + String(processor.getCurrentProgram()) + "|"
                   + processor.activeFileCartridge.getFullPathName();
        };
        // One MIDI CC through the processor, as a controller sends it.
        const auto sendCC = [&processor](int cc, int value) {
            AudioBuffer<float> audio(jmax(1, processor.getTotalNumOutputChannels()), 64);
            audio.clear();
            MidiBuffer midi;
            midi.addEvent(MidiMessage::controllerEvent(1, cc, value), 0);
            processor.processBlock(audio, midi);
        };
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
                    // The short rule after "AV Vibes" sits on the middle of the letters
                    // beside it ("ibes": the x-height), not well above it as it did
                    // (on the caps' middle). Measured on the render: the ink-weighted centre row of the
                    // rule and that of the "s" before it.
                    if (play && scale == 1.0f && String(locale) == "es") {
                        const auto identity = DexedAudioProcessorEditor::AVGetIdentityLayout();
                        GlyphArrangement letters;
                        letters.addLineOfText(AVDexedTheme::font(13.0f, "SemiBold"), "AV Vibes", identity.text.getX(), 0.0f);
                        const auto& lastLetter = letters.getGlyph(letters.getNumGlyphs() - 1);
                        const auto inkCentreRow = [&image](Rectangle<int> area) {
                            double weight = 0.0, rows = 0.0;
                            for (int y = area.getY(); y < area.getBottom(); ++y)
                                for (int x = area.getX(); x < area.getRight(); ++x) {
                                    const auto pixel = image.getPixelAt(x, y);
                                    const double ink = pixel.getBrightness() * pixel.getFloatAlpha();
                                    weight += ink;
                                    rows += ink * (y + 0.5);
                                }
                            return weight > 0.0 ? rows / weight : -100.0;
                        };
                        // Rows below the logo, through the whole line of text.
                        const int top = int(identity.text.getY()) + 2, bottom = int(identity.text.getBottom()) + 1;
                        const double ruleRow = inkCentreRow({ int(identity.rule.getX()) + 8, top, 40, bottom - top });
                        const double letterRow = inkCentreRow(Rectangle<float>(lastLetter.getLeft(), float(top),
                                                                               lastLetter.getRight() - lastLetter.getLeft(),
                                                                               float(bottom - top)).getSmallestIntegerContainer());
                        // This snapshot uses JUCE's software renderer, which puts a
                        // glyph's baseline on a whole pixel row (83 here, 82.66 by the
                        // font metrics that CoreGraphics draws at): the letters measure
                        // about 0.35 px lower than on screen, so the rule (centre 79,
                        // crisp at 100 %) reads about 0.65 px above the "s" here and
                        // about 0.3 px in the app. The old rule read 1.65 px above.
                        if (std::abs(ruleRow - letterRow) > 0.8) {
                            std::cerr << "Brand rule off the letters' middle: rule row " << ruleRow << ", letters " << letterRow << '\n';
                            passed = false;
                        }
                    }
                    const auto name = String(play ? "Play-" : "Advanced-") + locale + (scale == 1.0f ? "-normal.png" : "-compact.png");
                    auto output = directory.getChildFile(name).createOutputStream();
                    if (output) { output->setPosition(0); output->truncate(); }
                    passed &= output != nullptr && PNGImageFormat().writeImageToStream(image, *output);
                    std::cout << "UI evidence: " << name << " " << editor->getWidth() << "x" << editor->getHeight() << '\n';
                }
                // Layers: off, then an X816 stack with a split and a layer off.
                // The instrument comes back as it was (the checks below compare
                // the whole state with the one at the start).
                MemoryBlock beforeLayers;
                processor.getStateInformation(beforeLayers);
                for (bool layered : { false, true }) {
                    if (layered) {
                        processor.stackAVLayers(DexedAudioProcessor::AVLayerCount, 7.0f);
                        processor.getAVLayerSettings(6).channel = 2;
                        processor.getAVLayerSettings(7).lowNote = 60;
                        processor.setAVLayerEnabled(5, false);
                        processor.selectAVLayer(2);
                    } else {
                        processor.setAVLayerMode(false);
                    }
                    editor->showAVView(DexedAudioProcessorEditor::AVView::layers, false);
                    editor->layersView.setVisible(false);   // visibilityChanged() reads the processor
                    editor->layersView.setVisible(true);
                    editor->refreshPresetHeader();
                    checkBounds(editor->layersView);
                    for (auto* strip : editor->layersView.getChildren()) checkBounds(*strip);
                    auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
                    const auto name = String("Layers-") + (layered ? "on-" : "off-") + locale + (scale == 1.0f ? "-normal.png" : "-compact.png");
                    auto output = directory.getChildFile(name).createOutputStream();
                    if (output) { output->setPosition(0); output->truncate(); }
                    passed &= output != nullptr && PNGImageFormat().writeImageToStream(image, *output);
                    std::cout << "UI evidence: " << name << '\n';
                }
                // A real TX802 performance, when the factory files are on this machine.
                const File tx802(String::fromUTF8(std::getenv("AVDEXED_TX802_PERFORMANCES") ? std::getenv("AVDEXED_TX802_PERFORMANCES") : ""));
                if (tx802.existsAsFile()) {
                    editor->layersView.loadPerformanceFile(tx802);
                    editor->layersView.performanceList.setSelectedId(3, sendNotificationSync);
                    editor->layersView.setVisible(false);
                    editor->layersView.setVisible(true);
                    editor->refreshPresetHeader();
                    auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
                    const auto name = String("Layers-tx802-") + locale + (scale == 1.0f ? "-normal.png" : "-compact.png");
                    auto output = directory.getChildFile(name).createOutputStream();
                    if (output) { output->setPosition(0); output->truncate(); }
                    passed &= output != nullptr && PNGImageFormat().writeImageToStream(image, *output);
                }
                passed &= processor.loadAVStateInformation(beforeLayers.getData(), int(beforeLayers.getSize()));
                processor.keyboardState.noteOn(2, 60, 0.8f);   // a restore lets go of held keys; this is no UI change
                editor->showAVDexedPlayView(true, false);
            }
        }
        for (auto locale : {"es", "en"}) {
            AVDexedLanguageService::getInstance().setLanguage(locale);
            AVDexedLanguageService::getInstance().sendSynchronousChangeMessage();
            AVDexedSettingsPanel settings(processor, {});
            // Faders (general volume, BPM, instrument ranges, SysEx channel): the
            // value maps over exactly the span the track is drawn on, and the
            // thumb stays whole at both ends. The general volume rests at its
            // maximum (0 dB) and showed half a thumb 6 px past the track's end.
            std::function<void(Component&)> checkFaders = [&](Component& parent) {
                for (auto* child : parent.getChildren()) {
                    if (!child->isVisible()) continue;
                    if (auto* slider = dynamic_cast<Slider*>(child);
                        slider != nullptr && slider->getSliderStyle() == Slider::LinearHorizontal) {
                        const auto layout = slider->getLookAndFeel().getSliderLayout(*slider);
                        const auto track = DXLookNFeel::AVFaderTrack(layout.sliderBounds.toFloat());
                        const float radius = DXLookNFeel::AVFaderThumbRadius;
                        const auto travel = Rectangle<float>(track.getX() - radius, track.getCentreY() - radius,
                                                             track.getWidth() + 2.0f * radius, 2.0f * radius);
                        const bool aligned = std::abs(slider->getPositionOfValue(slider->getMinimum()) - track.getX()) < 0.01f
                                          && std::abs(slider->getPositionOfValue(slider->getMaximum()) - track.getRight()) < 0.01f;
                        const bool whole = slider->getLocalBounds().toFloat().contains(travel)
                                        && !layout.textBoxBounds.toFloat().intersects(travel);
                        if (!aligned || !whole) {
                            std::cerr << "Fader track/thumb mismatch: " << slider->getTitle() << " track "
                                      << track.toString() << " in " << slider->getLocalBounds().toString() << '\n';
                            passed = false;
                        }
                    }
                    checkFaders(*child);
                }
            };
            for (auto page : {AVDexedSettingsPanel::AVPage::audio, AVDexedSettingsPanel::AVPage::instrument,
                              AVDexedSettingsPanel::AVPage::midi, AVDexedSettingsPanel::AVPage::sysex,
                              AVDexedSettingsPanel::AVPage::about}) {
                settings.AVShowPage(page);
                checkFaders(settings);
                const auto filename = String("Settings-") + String(int(page)) + "-" + locale + ".png";
                auto output = directory.getChildFile(filename).createOutputStream();
                auto img = settings.createComponentSnapshot(settings.getLocalBounds());
                if (output) { output->setPosition(0); output->truncate(); }
                passed &= output != nullptr && PNGImageFormat().writeImageToStream(img, *output);
            }
        }
        // The progress dialog of an import that takes a while.
        for (const auto* locale : { "es", "en" }) {
            auto& language = AVDexedLanguageService::getInstance();
            language.setLanguage(locale);
            language.sendSynchronousChangeMessage();
            AVDexedPromptOverlay::Options options;
            options.title = language.text("importAction");
            options.message = language.format("importConverting", {{"folder", "Mis Rhodes"}, {"percent", "45"}});
            options.progress = [] { return 0.45f; };
            options.confirmText = language.text("cancel");
            options.returnConfirms = false;
            if (auto* prompt = editor->showAVPrompt(std::move(options), {})) {
                AVDexedComponentFade::cancel(*prompt);
                prompt->setVisible(true);
                prompt->setAlpha(1.0f);
                auto output = directory.getChildFile(String("Import-progress-") + locale + ".png").createOutputStream();
                const auto img = prompt->createComponentSnapshot(prompt->getLocalBounds());
                if (output) { output->setPosition(0); output->truncate(); }
                passed &= output != nullptr && PNGImageFormat().writeImageToStream(img, *output);
                prompt->dismiss();
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
        // Up / Down choose the previous / next sound anywhere in the editor and
        // never light a focus ring (only Tab does): an arrow key used to show the
        // ring of the Play tab, which a click on the top bar had focused.
        {
            auto& focusVisible = AVDexedTheme::keyboardFocusVisible();
            focusVisible = false;
            bool keys = ! AVDexedTheme::noteKeyboardNavigation(KeyPress(KeyPress::downKey))
                        && ! AVDexedTheme::noteKeyboardNavigation(KeyPress(KeyPress::rightKey)) && ! focusVisible
                        && AVDexedTheme::noteKeyboardNavigation(KeyPress(KeyPress::tabKey)) && focusVisible;
            focusVisible = false;
            // A click leaves no focus on a tab: empty space gives it to the frame.
            const bool clicks = editor->frameComponent.getWantsKeyboardFocus()
                                && ! editor->playTabButton.getMouseClickGrabsKeyboardFocus()
                                && ! editor->advancedTabButton.getMouseClickGrabsKeyboardFocus();
            editor->playView.navigatePreset(1);   // a sound of the list plays
            const auto first = currentSound();
            const bool down = editor->keyPressed(KeyPress(KeyPress::downKey), editor.get());
            const auto second = currentSound();
            const bool up = editor->keyPressed(KeyPress(KeyPress::upKey), editor.get());
            const auto third = currentSound();
            // With Cmd the key stays the host's.
            const bool command = editor->keyPressed(KeyPress(KeyPress::downKey, ModifierKeys::commandModifier, 0), editor.get());
            // A focused knob hands Up / Down over (the Play knobs are plain Sliders).
            const bool fromKnob = editor->AVSoundKeys.keyPressed(KeyPress(KeyPress::downKey), &editor->AVModWheel);
            const auto fourth = currentSound();
            editor->keyPressed(KeyPress(KeyPress::upKey), editor.get());
            const bool steps = down && up && ! command && fromKnob && second != first && third == first
                               && fourth == second && currentSound() == first && ! editor->hasAVPromptOpen();
            // The editor's own controls leave Up / Down alone; Left / Right still
            // step a knob (with Shift by 10 % of its range).
            DXSlider knob("AVArrowProbe");
            knob.setRange(0.0, 100.0, 1.0);
            knob.setValue(50.0, dontSendNotification);
            const bool controls = ! knob.keyPressed(KeyPress(KeyPress::upKey)) && ! knob.keyPressed(KeyPress(KeyPress::downKey))
                                  && approximatelyEqual(knob.getValue(), 50.0)
                                  && knob.keyPressed(KeyPress(KeyPress::rightKey)) && approximatelyEqual(knob.getValue(), 51.0)
                                  && knob.keyPressed(KeyPress(KeyPress::leftKey, ModifierKeys::shiftModifier, 0))
                                  && approximatelyEqual(knob.getValue(), 41.0)
                                  && ! editor->AVPitchWheel.keyPressed(KeyPress(KeyPress::upKey))
                                  && ! editor->global.programs->keyPressed(KeyPress(KeyPress::downKey));
            keys = keys && ! focusVisible;
            // The pitch wheel reads as a bend in the footer: 0 at rest.
            auto& pitch = editor->AVPitchWheel;
            const bool bend = pitch.getTextFromValue(8192.0) == "0" && pitch.getTextFromValue(16383.0) == "+8191"
                              && pitch.getTextFromValue(0.0) == String::fromUTF8("−8192")
                              && approximatelyEqual(pitch.getValueFromText("+100"), 8292.0)
                              && approximatelyEqual(pitch.getValueFromText(String::fromUTF8("−8192")), 0.0);
            if (! (keys && clicks && steps && controls && bend)) {
                std::cerr << "Arrow keys / pitch text: rings " << keys << ", clicks " << clicks << ", sound steps " << steps
                          << ", controls " << controls << ", pitch bend text " << bend << '\n';
                passed = false;
            }
            if (auto* prompt = dynamic_cast<AVDexedPromptOverlay*>(Component::getCurrentlyModalComponent()))
                prompt->keyPressed(KeyPress(KeyPress::escapeKey));
            passed &= processor.loadAVStateInformation(initial.getData(), int(initial.getSize()));
        }
        editor.reset();
        // Editor recreation is a separate path from state restore / app restart.
        editor = std::make_unique<DexedAudioProcessorEditor>(&processor);
        processor.getStateInformation(after);
        passed &= initial == after && processor.getAVPresetName() == String::fromUTF8("Órbita oceánica – AV");
        // Dialogs opened from inside the editor are its own prompts, never
        // desktop AlertWindows: a notice (device, MIDI port, profile and
        // tuning-file errors go through AVShowMessage), the "Save profile"
        // name prompt over Settings, and an error raised by a Settings page.
        {
            const auto promptOnTop = [] {
                return dynamic_cast<AVDexedPromptOverlay*>(Component::getCurrentlyModalComponent()) != nullptr;
            };
            const auto dismissPrompt = [] {
                if (auto* prompt = dynamic_cast<AVDexedPromptOverlay*>(Component::getCurrentlyModalComponent()))
                    prompt->keyPressed(KeyPress(KeyPress::escapeKey));
            };
            AVShowMessage(MessageBoxIconType::WarningIcon, "Notice", "Shown in the editor", &editor->playView);
            const bool noticeInEditor = editor->hasAVPromptOpen() && promptOnTop();
            dismissPrompt();
            editor->parmShow();
            ParamDialog* instrument = nullptr;
            std::function<void(Component&)> findInstrument = [&](Component& parent) {
                for (auto* child : parent.getChildren()) {
                    if (instrument == nullptr) instrument = dynamic_cast<ParamDialog*>(child);
                    if (instrument == nullptr) findInstrument(*child);
                }
            };
            findInstrument(*editor);
            bool profilePrompt = false, settingsNotice = false;
            if (instrument != nullptr) {
                instrument->AVAskProfileName();
                profilePrompt = editor->hasAVPromptOpen() && promptOnTop();
                dismissPrompt();
                AVShowMessage(MessageBoxIconType::WarningIcon, "Notice", "Shown over Settings", instrument);
                settingsNotice = editor->hasAVPromptOpen() && promptOnTop();
                dismissPrompt();
            }
            // The prompt's validator: usable names pass; empty, path, hidden,
            // reserved and Windows device names keep it open with a message.
            bool names = ParamDialog::AVProfileNameProblem("Mi perfil").isEmpty()
                         && ParamDialog::AVProfileNameProblem("  Rhodes 2  ").isEmpty()
                         && ParamDialog::AVProfileNameProblem("Profiles").isEmpty();
            for (auto bad : { "", "   ", "a/b", "a\\b", "a:b", ".oculto", "..", "Fin.", "a?b", "Con", "lpt1.txt" })
                names = names && ParamDialog::AVProfileNameProblem(bad).isNotEmpty();
            if (!(noticeInEditor && profilePrompt && settingsNotice && names)) {
                std::cerr << "In-editor dialogs: notice " << noticeInEditor << ", profile prompt " << profilePrompt
                          << ", notice over Settings " << settingsNotice << ", profile names " << names << '\n';
                passed = false;
            }
            // "AV Dexed" in Advanced shows About inside the editor: Settings
            // (modal, blurred, fixed) turned to its About page; Settings is
            // still open from the checks above, so this also turns an open one.
            const auto findInOverlay = [&editor](const std::function<bool(Component&)>& match) {
                Component* found = nullptr;
                std::function<void(Component&)> walk = [&](Component& parent) {
                    for (auto* child : parent.getChildren()) {
                        if (found == nullptr && match(*child)) found = child;
                        if (found == nullptr) walk(*child);
                    }
                };
                if (editor->AVSettingsOverlay != nullptr) walk(*editor->AVSettingsOverlay);
                return found;
            };
            const auto closeSettings = [&editor] {
                if (editor->AVSettingsOverlay != nullptr && editor->AVSettingsOverlay->isVisible())
                    editor->AVSettingsOverlay->keyPressed(KeyPress(KeyPress::escapeKey));
            };
            bool about = false;
            if (editor->global.onAboutRequested) {
                editor->global.onAboutRequested();
                auto* panel = dynamic_cast<AVDexedSettingsPanel*>(findInOverlay([](Component& c) {
                    return dynamic_cast<AVDexedSettingsPanel*>(&c) != nullptr;
                }));
                about = panel != nullptr && panel->AVGetPage() == AVDexedSettingsPanel::AVPage::about
                        && Component::getCurrentlyModalComponent() == editor->AVSettingsOverlay.get();
            }
            closeSettings();
            // MIDI Learn of the next-sound arrow: the in-editor waiting dialog,
            // closed by the CC that arrives, which the footer then names. A press
            // of that control (from below 64 to 64 or above) is the next sound,
            // and Settings lists the assignment.
            using Step = DexedAudioProcessor::AVPresetStep;
            editor->playView.onPresetStepLearnRequested(1);
            const bool waiting = editor->hasAVPromptOpen() && promptOnTop() && editor->AVStepLearn == 1;
            sendCC(20, 127);
            processor.midiCCMapper->commitAVPresetStepLearn();   // the CC handler's timer does it at 30 Hz
            editor->AVFollowPresetStepCCs();
            const bool learned = processor.getAVPresetStepCC(Step::next) == 20 && ! editor->hasAVPromptOpen()
                                 && editor->AVStepLearn < 0 && editor->AVNoticeText.contains("CC 20");
            const auto beforePress = currentSound();
            sendCC(20, 0);
            sendCC(20, 127);
            editor->AVFollowPresetStepCCs();
            // The recreated editor may still be reading the library: then the
            // press must wait for the list (kept, not dropped) instead of moving.
            const bool pressed = editor->playView.isWaitingForFirstIndex() ? editor->AVPendingSoundSteps == 1
                                                                           : currentSound() != beforePress;
            editor->showAVSettingsPage(AVDexedSettingsPanel::AVPage::midi);
            const auto rowTitle = AVDexedLanguageService::getInstance().text("nextSound") + " · CC 20";
            const bool listed = findInOverlay([&rowTitle](Component& c) { return c.getTitle() == rowTitle; }) != nullptr;
            closeSettings();
            processor.clearAVPresetStepCC(Step::next);
            // A knob's "Asignar CC MIDI" (its right-click menu) uses the same
            // in-editor dialog, never a desktop window.
            bool knobLearn = false;
            for (auto* child : editor->global.getChildren())
                if (auto* resonance = dynamic_cast<Slider*>(child); resonance != nullptr && child->getName() == IDs::resonance.name) {
                    new MidiCCListenerStatus(resonance, processor.midiCCMapper.get(), processor.parameters.getParameter(IDs::resonance.name));
                    const bool knobWaiting = editor->hasAVPromptOpen() && promptOnTop();
                    sendCC(21, 90);
                    processor.midiCCMapper->commitAVPendingMapping();
                    knobLearn = knobWaiting && ! editor->hasAVPromptOpen() && editor->AVNoticeText.contains("CC 21")
                                && processor.midiCCMapper->getAVMappingParameterId(21) == String(IDs::resonance.name);
                    processor.midiCCMapper->removeMapping(21);
                }
            if (! (about && waiting && learned && pressed && listed && knobLearn)) {
                std::cerr << "About / MIDI Learn: About page " << about << ", arrow dialog " << waiting << ", learned " << learned
                          << ", pedal step " << pressed << ", listed in Settings " << listed << ", knob dialog " << knobLearn << '\n';
                passed = false;
            }
        }
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
        };
        Timer::callAfterDelay(2200, render);
#if JUCE_MAC
        // Console apps have no NSApp run loop. Pump JUCE's CF message source;
        // otherwise a deferred renderer could falsely report success without running.
        const auto deadline = Time::getMillisecondCounterHiRes() + 30000;
        while (!completed && Time::getMillisecondCounterHiRes() < deadline)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
#else
        MessageManager::getInstance()->runDispatchLoop();
#endif
        passed &= completed;
    }
    DexedAudioProcessor::dexedAppDir = oldAppDirectory;
    DexedAudioProcessor::dexedCartDir = oldCartDirectory;
    // Only the temporary root this run created (the PNGs stay in `directory`).
    if (libraryRoot.isAChildOf(File::getSpecialLocation(File::tempDirectory))
        && libraryRoot.getFileName().startsWith("avdexed-uievidence-"))
        libraryRoot.deleteRecursively();
    // A test binary never resolved a data folder: then nothing is written back.
    AVDexedLanguageService::getInstance().configure(oldAppDirectory == File() ? File()
                                                    : oldAppDirectory.getChildFile(DexedAudioProcessor::preferencesFileName));
    AVDexedLanguageService::getInstance().setLanguage(oldLanguage);
    std::cout << (passed ? "UI geometry, state/view invariance and SVG checks passed.\n" : "UI QA failed.\n");
    return passed;
}
