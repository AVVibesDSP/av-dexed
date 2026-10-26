// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// --ui-cpu <bank.syx> <slot>: how much CPU the editor and the audio take in a
// real window, as when playing. A thread renders audio in real time (128
// samples at 48 kHz) while the editor shows each page; the process CPU minus
// the audio thread's is what the editor costs (the window server composites
// on top of that, in its own process). The message thread's own share (where
// the editor paints) is printed apart.
//
// --advanced-notice <folder>: opens Advanced in a real window: the notice
// that Advanced draws more while notes sound shows once (Spanish), "Don't show
// again" is chosen and saved, and Advanced opens again without it; then the
// English notice. The captures go to <folder>.
//
// --ui-resize <bank.syx> <slot> <folder>: the window is resized as the grip
// does (zoom 1.0 → 0.8 → 1.6 → 1.25) while eight layers sound, then the keys
// go up and everything settles. The window as the screen shows it then must
// equal the same window after a full repaint: a region that was not redrawn
// (or drawn black) after the resize would differ. Then the same window is
// painted with no cached images or text (AVDexedPixelCache and
// AVDexedTheme::TextCache off): it must look the same. The captures go to <folder>.
//
// --ui-fades <bank.syx> <slot>: the tab change, Settings and a dialog fade in
// and out many times in a real window; how smoothly and at what cost (see
// AVRunUIFades below).

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <unistd.h>
#include <mach/mach.h>
#endif
#include "../source/ui/PluginEditor.h"
#include "../source/ui/AVDexedPixelCache.h"
#include "../source/core/AVDexedLibrary.h"
#include "../source/parameter/Model.h"
#include <atomic>
#include <chrono>
#include <ctime>
#include <iostream>
#include <map>
#include <thread>
#include <sys/resource.h>

namespace {
double processSeconds() {
    rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1.0e-6;
}
double threadSeconds() {
    timespec now {};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return double(now.tv_sec) + double(now.tv_nsec) * 1.0e-9;
}
// The calling thread's CPU in the kernel only (macOS), in seconds.
double threadSystemSeconds() {
#if defined(__APPLE__)
    thread_basic_info_data_t info {};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    const auto self = mach_thread_self();
    const bool ok = thread_info(self, THREAD_BASIC_INFO, (thread_info_t) &info, &count) == KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(), self);
    return ok ? double(info.system_time.seconds) + double(info.system_time.microseconds) * 1.0e-6 : 0.0;
#else
    return 0.0;
#endif
}

// An isolated library, as --render-ui: the user's folders are never read or written.
struct IsolatedLibrary {
    IsolatedLibrary() {
        using namespace juce;
        root.createDirectory();
        DexedAudioProcessor::dexedAppDir = root;
        DexedAudioProcessor::dexedCartDir = AVDexedLibrary::rootFor(root);
        AVDexedLibrary::installFactorySounds(DexedAudioProcessor::dexedCartDir, BinaryData::builtin_pgm_zip,
                                             (size_t) BinaryData::builtin_pgm_zipSize, false);
    }
    ~IsolatedLibrary() {
        using namespace juce;
        DexedAudioProcessor::dexedAppDir = oldAppDirectory;
        DexedAudioProcessor::dexedCartDir = oldCartDirectory;
        if (root.isAChildOf(File::getSpecialLocation(File::tempDirectory)) && root.getFileName().startsWith("avdexed-uicpu-"))
            root.deleteRecursively();
    }
    const juce::File oldAppDirectory = DexedAudioProcessor::dexedAppDir;
    const juce::File oldCartDirectory = DexedAudioProcessor::dexedCartDir;
    const juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile("avdexed-uicpu-" + juce::Uuid().toString());
};

// The audio thread, paced in real time.
struct RealTimeAudio {
    static constexpr double rate = 48000.0;
    static constexpr int block = 128;
    explicit RealTimeAudio(DexedAudioProcessor& processorToRun) : processor(processorToRun) {
        processor.setRateAndBufferSizeDetails(rate, block);
        processor.prepareToPlay(rate, block);
        thread = std::thread([this] {
            juce::AudioBuffer<float> buffer(2, block);
            auto next = std::chrono::steady_clock::now();
            const auto period = std::chrono::nanoseconds(juce::int64(1.0e9 * block / rate));
            while (running.load()) {
                const double start = threadSeconds();
                juce::MidiBuffer midi;
                buffer.clear();
                processor.processBlock(buffer, midi);
                seconds.store(seconds.load() + threadSeconds() - start);
                next += period;
                std::this_thread::sleep_until(next);
            }
        });
    }
    ~RealTimeAudio() {
        running.store(false);
        thread.join();
    }
    DexedAudioProcessor& processor;
    std::atomic<bool> running { true };
    std::atomic<double> seconds { 0.0 };
    std::thread thread;
};

bool loadSound(DexedAudioProcessor& processor, const juce::File& bankFile, int slot) {
    Cartridge bank;
    if (bank.load(bankFile) < 0) return false;
    processor.setActiveBank(bank, bankFile);
    processor.setCurrentProgram(juce::jlimit(0, 31, slot - 1));
    return true;
}

// Sixteen keys held: with eight layers every voice of every layer sounds.
void playSixteenKeys(DexedAudioProcessor& processor, bool on) {
    for (int n = 0; n < 16; ++n) {
        if (on) processor.keyboardState.noteOn(1, 36 + n * 3, 0.8f);
        else processor.keyboardState.noteOff(1, 36 + n * 3, 0.0f);
    }
}

#if defined(__APPLE__)
// The window server's number for this process's normal window, or 0.
int ownWindowNumber() {
    int found = 0;
    if (auto list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID)) {
        for (CFIndex i = 0; i < CFArrayGetCount(list) && found == 0; ++i) {
            auto info = (CFDictionaryRef) CFArrayGetValueAtIndex(list, i);
            int pid = 0, layer = -1, number = 0;
            if (auto value = (CFNumberRef) CFDictionaryGetValue(info, kCGWindowOwnerPID)) CFNumberGetValue(value, kCFNumberIntType, &pid);
            if (auto value = (CFNumberRef) CFDictionaryGetValue(info, kCGWindowLayer)) CFNumberGetValue(value, kCFNumberIntType, &layer);
            if (auto value = (CFNumberRef) CFDictionaryGetValue(info, kCGWindowNumber)) CFNumberGetValue(value, kCFNumberIntType, &number);
            if (pid == getpid() && layer == 0) found = number;
        }
        CFRelease(list);
    }
    return found;
}
#endif

// The window as the screen shows it (title bar included), at the display's resolution.
juce::Image captureWindow(const juce::File& file) {
    using namespace juce;
#if defined(__APPLE__)
    const int number = ownWindowNumber();
    if (number == 0) return {};
    file.deleteFile();
    ChildProcess capture;
    if (!capture.start(StringArray { "/usr/sbin/screencapture", "-x", "-o", "-l" + String(number), file.getFullPathName() }))
        return {};
    capture.waitForProcessToFinish(15000);
    return ImageFileFormat::loadFrom(file);
#else
    ignoreUnused(file);
    return {};
#endif
}
}

int AVRunUICpu(const juce::File& bankFile, int slot) {
    using namespace juce;
    IsolatedLibrary library;
    int result = 0;
    {
        DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
        if (!loadSound(processor, bankFile, slot)) return 2;
        processor.advancedNoticeHidden = true;   // its dialog would cover Advanced
        RealTimeAudio audio(processor);

        // Counts the editor's real paints: a see-through cover over the whole
        // editor is painted whenever any part under it is.
        static std::atomic<int> paints { 0 };
        struct Counter : Component {
            Counter() { setInterceptsMouseClicks(false, false); }
            void paint(Graphics&) override { paints.fetch_add(1); }
        };
        DocumentWindow window("AV Dexed UI CPU", Colours::black, DocumentWindow::closeButton);
        auto* editor = new DexedAudioProcessorEditor(&processor);
        window.setUsingNativeTitleBar(true);
        window.setContentOwned(editor, true);
        window.setTopLeftPosition(80, 80);
        Counter counter;
        editor->addAndMakeVisible(counter);
        counter.setBounds(editor->getLocalBounds());
        counter.setAlwaysOnTop(true);

        struct Step { const char* name; std::function<void()> setUp; };
        const auto play = [&](bool on) { playSixteenKeys(processor, on); };
        const auto effects = [&](bool on) {
            for (auto [id, value] : { std::pair { IDs::reverbAmount.parameter(), 0.3f }, { IDs::chorusAmount.parameter(), 0.5f },
                                      { IDs::delayAmount.parameter(), 0.2f } })
                if (auto* parameter = processor.parameters.getParameter(id.getParamID()))
                    parameter->setValueNotifyingHost(on ? value : 0.0f);
        };
        // An arpeggio, eight notes a second over two octaves: the keyboard and
        // the glow change on every note.
        bool arpeggiating = false;
        int arpeggioStep = 0;
        std::function<void()> arpeggio;
        arpeggio = [&] {
            if (!arpeggiating) return;
            static constexpr int pattern[] { 48, 52, 55, 60, 64, 67, 72, 67, 64, 60, 55, 52 };
            const int count = int(std::size(pattern));
            processor.keyboardState.noteOff(1, pattern[(arpeggioStep + count - 1) % count], 0.0f);
            processor.keyboardState.noteOn(1, pattern[arpeggioStep % count], 0.8f);
            ++arpeggioStep;
            Timer::callAfterDelay(125, arpeggio);
        };
        // A stack starts by silencing every layer: the keys go down a moment after.
        const auto stackThenPlay = [&] {
            play(false);
            processor.stackAVLayers(8, 7.0f);
            Timer::callAfterDelay(300, [&] { play(true); });
        };
        std::vector<Step> steps {
            { "window hidden, silent", [&] { window.setVisible(false); } },
            { "Play page, silent", [&] { window.setVisible(true); editor->showAVView(DexedAudioProcessorEditor::AVView::play, false); } },
            { "Play page, 1 layer, 16 keys", [&] { play(true); } },
            { "Play page, 8 layers, 16 keys", stackThenPlay },
            { "Advanced page, 8 layers, 16 keys", [&] { editor->showAVView(DexedAudioProcessorEditor::AVView::advanced, false); } },
            { "Layers page, 8 layers, 16 keys", [&] { editor->showAVView(DexedAudioProcessorEditor::AVView::layers, false); } },
            { "Layers page, 8 layers, 16 keys, effects", [&] { effects(true); } },
            // Covered: nothing of what moves underneath can be seen. (A minimised
            // window cannot be had here: a console app's windows do not minimise.)
            { "Play page, 8 layers, effects, Settings open", [&] {
                editor->showAVView(DexedAudioProcessorEditor::AVView::play, false);
                editor->showAVSettingsPage(AVDexedSettingsPanel::AVPage::instrument);
            } },
            { "Play page, 8 layers, effects, arpeggio", [&] {
                editor->AVCloseSettings(true);
                play(false);
                arpeggiating = true;
                arpeggio();
            } },
            { "Play page, 8 layers, effects, arpeggio, low power", [&] {
                AVDexedTheme::lowPower = true;
                editor->repaint();
            } },
            { "window hidden, 8 layers, effects", [&] {
                AVDexedTheme::lowPower = false;
                arpeggiating = false;
                processor.keyboardState.allNotesOff(1);
                play(true);
                window.setVisible(false);
            } },
        };
        constexpr double settle = 1.5, measure = 5.0;
        size_t index = 0;
        double phaseStart = 0.0, cpuStart = 0.0, audioStart = 0.0, messageStart = 0.0;
        bool measuring = false, finished = false;
        std::function<void()> tick;
        tick = [&] {
            const double now = Time::getMillisecondCounterHiRes() / 1000.0;
            if (! measuring && now - phaseStart >= settle) {
                measuring = true;
                phaseStart = now;
                cpuStart = processSeconds();
                audioStart = audio.seconds.load();
                messageStart = threadSeconds();   // this runs on the message thread
            } else if (measuring && now - phaseStart >= measure) {
                const double total = (processSeconds() - cpuStart) / measure * 100.0;
                const double audioPart = (audio.seconds.load() - audioStart) / measure * 100.0;
                const double messagePart = (threadSeconds() - messageStart) / measure * 100.0;
                std::cout << String(steps[index].name).paddedRight(' ', 40) << " process " << String(total, 1).paddedLeft(' ', 5)
                          << " %   audio " << String(audioPart, 1).paddedLeft(' ', 5) << " %   rest (editor) "
                          << String(total - audioPart, 1).paddedLeft(' ', 5) << " % (message thread "
                          << String(messagePart, 1) << " %)  of one core, "
                          << processor.getAVActiveVoices() << " voices, " << paints.load() << " paints, footer CPU "
                          << String(processor.getAVAudioCpuPercent(), 1) << " %\n";
                paints.store(0);
                if (++index == steps.size()) {
                    finished = true;
                    MessageManager::getInstance()->stopDispatchLoop();
                    return;
                }
                steps[index].setUp();
                measuring = false;
                phaseStart = now;
            }
            Timer::callAfterDelay(50, tick);
        };
        steps[0].setUp();
        phaseStart = Time::getMillisecondCounterHiRes() / 1000.0;
        Timer::callAfterDelay(50, tick);
        // The application's own loop, so the window draws as an app's does.
        MessageManager::getInstance()->runDispatchLoop();
        result = finished ? 0 : 1;
        play(false);
        window.setVisible(false);
        editor->removeChildComponent(&counter);
        window.clearContentComponent();
    }
    return result;
}

int AVRunUIResize(const juce::File& bankFile, int slot, const juce::File& outputDirectory) {
    using namespace juce;
    outputDirectory.createDirectory();
    IsolatedLibrary library;
    bool passed = false;
    {
        DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
        if (!loadSound(processor, bankFile, slot)) return 2;
        RealTimeAudio audio(processor);
        DocumentWindow window("AV Dexed UI resize", Colours::black, DocumentWindow::closeButton);
        auto* editor = new DexedAudioProcessorEditor(&processor);
        window.setUsingNativeTitleBar(true);
        window.setContentOwned(editor, true);
        window.setTopLeftPosition(80, 80);
        window.setVisible(true);
        editor->showAVView(DexedAudioProcessorEditor::AVView::play, false);
        processor.stackAVLayers(8, 7.0f);

        // The grip's path: small steps every 33 ms, as a drag delivers them.
        std::vector<float> zooms;
        for (float z = 1.0f; z > 0.8f; z -= 0.02f) zooms.push_back(z);
        for (float z = 0.8f; z < 1.6f; z += 0.02f) zooms.push_back(z);
        for (float z = 1.6f; z > 1.25f; z -= 0.02f) zooms.push_back(z);
        zooms.push_back(1.25f);

        size_t step = 0;
        Image settled, repainted, repaintedAgain, direct;
        double releasedAt = 0.0;
        std::function<void()> resize, release, waitUntilStill, compare;
        const auto after = [&](int milliseconds, std::function<void()> then) {
            Timer::callAfterDelay(milliseconds, [&, then] { then(); });
        };
        const auto finish = [&] { MessageManager::getInstance()->stopDispatchLoop(); };
        resize = [&] {
            if (step < zooms.size()) {
                processor.setZoomFactor(zooms[step++]);
                editor->resetSize();
                after(33, resize);
                return;
            }
            // Still sounding at the new size: the glow and the meter keep drawing.
            after(1000, release);
        };
        release = [&] {
            captureWindow(outputDirectory.getChildFile("resize-playing.png"));
            playSixteenKeys(processor, false);
            releasedAt = Time::getMillisecondCounterHiRes();
            after(500, waitUntilStill);
        };
        // Every animation ends: the glow fades, the voices end and the meter
        // falls to its floor (eight layers ring on for seconds).
        waitUntilStill = [&] {
            const bool still = processor.getAVActiveVoices() == 0 && editor->AVOutputMeter.AVDisplayedGain() <= 0.0f;
            if (still || Time::getMillisecondCounterHiRes() - releasedAt > 30000.0) after(1000, compare);
            else after(200, waitUntilStill);
        };
        compare = [&] {
            settled = captureWindow(outputDirectory.getChildFile("resize-settled.png"));
            editor->repaint();
            after(600, [&] {
                repainted = captureWindow(outputDirectory.getChildFile("resize-repainted.png"));
                editor->repaint();
                after(600, [&] {
                    repaintedAgain = captureWindow(outputDirectory.getChildFile("resize-repainted-again.png"));
                    AVDexedPixelCache::enabled = false;
                    AVDexedTheme::TextCache::enabled = false;
                    editor->repaint();
                    after(600, [&] {
                        direct = captureWindow(outputDirectory.getChildFile("resize-direct.png"));
                        AVDexedPixelCache::enabled = true;
                        AVDexedTheme::TextCache::enabled = true;
                        finish();
                    });
                });
            });
        };
        after(300, [&] {
            playSixteenKeys(processor, true);
            after(1200, resize);
        });
        MessageManager::getInstance()->runDispatchLoop();

        // Two full repaints in a row: what differs between them is the renderer's
        // own noise (gradients are dithered), not something left undrawn.
        // Pixels that differ, and by how many levels at most.
        const auto differences = [](const Image& a, const Image& b) {
            int count = 0, largest = 0;
            if (a.isValid() && b.isValid() && a.getBounds() == b.getBounds())
                for (int y = 0; y < a.getHeight(); ++y)
                    for (int x = 0; x < a.getWidth(); ++x) {
                        const auto p = a.getPixelAt(x, y), q = b.getPixelAt(x, y);
                        const int change = jmax(std::abs(p.getRed() - q.getRed()), std::abs(p.getGreen() - q.getGreen()),
                                                std::abs(p.getBlue() - q.getBlue()));
                        if (change > 0) { ++count; largest = jmax(largest, change); }
                    }
            return String(count) + " px (at most " + String(largest) + " levels)";
        };
        if (settled.isNull() || repainted.isNull() || settled.getBounds() != repainted.getBounds()) {
            std::cout << "UI resize: the window could not be captured (screen recording permission?)\n";
        } else {
            // The footer's CPU and voice figures change by themselves; they are
            // reported apart. Rows are mapped to the editor's own coordinates.
            // A black key's shadow is blurred within the area being painted, so
            // it may differ by a few levels: up to 16 of 255 is reported, not failed.
            const float pixelsPerPoint = float(settled.getWidth()) / float(editor->getWidth());
            const int titleBar = settled.getHeight() - roundToInt(editor->getHeight() * pixelsPerPoint);
            const float footerTop = 905.0f * processor.getZoomFactor() * pixelsPerPoint + float(titleBar);
            int different = 0, slight = 0, footer = 0;
            Rectangle<int> area;
            for (int y = 0; y < settled.getHeight(); ++y)
                for (int x = 0; x < settled.getWidth(); ++x) {
                    const auto a = settled.getPixelAt(x, y), b = repainted.getPixelAt(x, y);
                    const int change = jmax(std::abs(a.getRed() - b.getRed()), std::abs(a.getGreen() - b.getGreen()),
                                            std::abs(a.getBlue() - b.getBlue()));
                    if (change == 0) continue;
                    if (float(y) >= footerTop) { ++footer; continue; }
                    if (change <= 16) { ++slight; continue; }
                    ++different;
                    area = area.isEmpty() ? Rectangle<int>(x, y, 1, 1) : area.getUnion({ x, y, 1, 1 });
                }
            passed = different == 0;
            std::cout << "UI resize: zoom " << String(processor.getZoomFactor(), 2) << ", window " << settled.getWidth() << "x"
                      << settled.getHeight() << " px, " << different << " px differ from a full repaint"
                      << (different > 0 ? " in " + area.toString() : String()) << " (" << slight << " by 16 or less, "
                      << footer << " in the footer figures); two full repaints differ in "
                      << differences(repainted, repaintedAgain) << "; with no cached images: "
                      << differences(repaintedAgain, direct) << "\n";
        }
        window.setVisible(false);
        window.clearContentComponent();
    }
    return passed ? 0 : 1;
}

int AVRunAdvancedNotice(const juce::File& outputDirectory) {
    using namespace juce;
    outputDirectory.createDirectory();
    IsolatedLibrary library;
    auto& language = AVDexedLanguageService::getInstance();
    const auto oldLanguage = language.getLanguage();
    // Language changes are saved: to the isolated library's preferences only.
    language.configure(library.root.getChildFile(DexedAudioProcessor::preferencesFileName));
    bool passed = true;
    const auto check = [&passed](bool condition, const char* what) {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << "\n";
        passed &= condition;
    };
    // The dialog's pill with this text, if it is up.
    const auto findPill = [](Component& root, const String& text) -> Button* {
        std::function<Button*(Component&)> search = [&](Component& c) -> Button* {
            for (auto* child : c.getChildren()) {
                if (auto* button = dynamic_cast<TextButton*>(child))
                    if (button->isShowing() && button->getButtonText() == text) return button;
                if (auto* found = search(*child)) return found;
            }
            return nullptr;
        };
        return search(root);
    };
    {
        DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
        DocumentWindow window("AV Dexed notice", Colours::black, DocumentWindow::closeButton);
        window.setUsingNativeTitleBar(true);
        window.setTopLeftPosition(80, 80);
        const auto open = [&](const char* locale) {
            language.setLanguage(locale);
            language.sendSynchronousChangeMessage();
            auto* editor = new DexedAudioProcessorEditor(&processor);
            window.setContentOwned(editor, true);
            window.setVisible(true);
            editor->applyLanguage();
            editor->showAVView(DexedAudioProcessorEditor::AVView::play, false);
            return editor;
        };
        // Each stage runs after the previous one's delay, in the app's own loop.
        // The tab's click is AVOpenAdvanced(true, {}).
        DexedAudioProcessorEditor* editor = nullptr;
        const auto onPage = [&editor](DexedAudioProcessorEditor::AVView view) { return editor->AVVisibleView == view; };
        const auto click = [&](const char* key) {
            auto* pill = findPill(*editor, language.text(key));
            check(pill != nullptr, key);
            if (pill != nullptr) pill->triggerClick();
        };
        std::vector<std::pair<int, std::function<void()>>> stages {
            { 0, [&] {
                processor.advancedNoticeHidden = false;
                editor = open("es");
                editor->AVOpenAdvanced(true, {});
            } },
            { 900, [&] {
                captureWindow(outputDirectory.getChildFile("advanced-notice-es.png"));
                check(editor->hasAVPromptOpen(), "the question shows the first time Advanced is clicked");
                check(onPage(DexedAudioProcessorEditor::AVView::play), "before Advanced opens: Play stays behind it");
                click("cancel");
            } },
            { 600, [&] {
                check(!editor->hasAVPromptOpen() && onPage(DexedAudioProcessorEditor::AVView::play), "\"Cancelar\" stays in Play");
                editor->AVOpenAdvanced(true, {});
            } },
            { 900, [&] {
                check(editor->hasAVPromptOpen(), "the next click asks again");
                click("advancedOpen");
            } },
            { 600, [&] {
                check(!editor->hasAVPromptOpen() && onPage(DexedAudioProcessorEditor::AVView::advanced) && !processor.advancedNoticeHidden,
                      "\"Abrir\" opens Advanced");
                editor->showAVView(DexedAudioProcessorEditor::AVView::play, true);
            } },
            { 400, [&] { editor->AVOpenAdvanced(true, {}); } },
            { 900, [&] {
                check(!editor->hasAVPromptOpen() && onPage(DexedAudioProcessorEditor::AVView::advanced),
                      "then it opens without asking while this window lasts");
                editor = open("en");
                editor->AVOpenAdvanced(true, {});
            } },
            { 900, [&] {
                captureWindow(outputDirectory.getChildFile("advanced-notice-en.png"));
                check(editor->hasAVPromptOpen() && onPage(DexedAudioProcessorEditor::AVView::play), "a new window asks again, in English too");
                click("advancedOpenAlways");
            } },
            { 600, [&] {
                check(!editor->hasAVPromptOpen() && onPage(DexedAudioProcessorEditor::AVView::advanced), "\"Always open\" opens Advanced");
                check(processor.advancedNoticeHidden, "and is remembered");
                processor.advancedNoticeHidden = false;
                processor.loadPreference();
                check(processor.advancedNoticeHidden, "and saved with the preferences");
                editor = open("es");
                editor->AVOpenAdvanced(true, {});
            } },
            { 900, [&] {
                check(!editor->hasAVPromptOpen() && onPage(DexedAudioProcessorEditor::AVView::advanced), "a new window then opens it at once");
                MessageManager::getInstance()->stopDispatchLoop();
            } },
        };
        size_t stage = 0;
        std::function<void()> next;
        next = [&] {
            stages[stage].second();
            if (++stage < stages.size()) Timer::callAfterDelay(stages[stage].first, next);
        };
        Timer::callAfterDelay(100, next);
        MessageManager::getInstance()->runDispatchLoop();
        window.setVisible(false);
        window.clearContentComponent();
    }
    // As --render-ui: a test binary never resolved a data folder, so nothing is written back.
    language.configure(library.oldAppDirectory == File() ? File()
                       : library.oldAppDirectory.getChildFile(DexedAudioProcessor::preferencesFileName));
    language.setLanguage(oldLanguage);
    language.sendSynchronousChangeMessage();
    std::cout << (passed ? "Advanced notice checks passed.\n" : "Advanced notice checks FAILED.\n");
    return passed ? 0 : 1;
}

/** --ui-fades <bank.syx> <slot>: how the editor's fades run in a real window:
    the tab change, Settings and an in-editor dialog, each opened and closed
    several times, silent and with eight layers, effects and 16 keys sounding,
    at zoom 1.0 and 0.83. For each kind: how long the click itself takes on the
    message thread, how long until the first frame is painted, how many of the
    screen's frames get painted during the fade, the longest stretch without a
    painted frame (in screen frames: 1 is every frame), what painting a frame
    costs and the message thread's CPU for the whole fade. */
int AVRunUIFades(const juce::File& bankFile, int slot) {
    using namespace juce;
    using Ms = std::chrono::duration<double, std::milli>;
    IsolatedLibrary library;
    int result = 1;
    {
        DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
        if (!loadSound(processor, bankFile, slot)) return 2;
        processor.advancedNoticeHidden = true;
        RealTimeAudio audio(processor);
        DocumentWindow window("AV Dexed UI fades", Colours::black, DocumentWindow::closeButton);
        auto* editor = new DexedAudioProcessorEditor(&processor);
        window.setUsingNativeTitleBar(true);
        window.setContentOwned(editor, true);
        window.setTopLeftPosition(80, 80);
        window.setVisible(true);
        editor->showAVView(DexedAudioProcessorEditor::AVView::play, false);

        // Every paint pass of the window (one per dirty area), and the screen's frames.
        struct PaintLog : ComponentListener {
            std::vector<std::pair<double, double>> paints;   // end, duration (ms)
            void componentPainted(Component&, const ComponentPaintDiagnostics& d) override {
                paints.emplace_back(Time::getMillisecondCounterHiRes(), d.totalPaintDuration.get<Ms>());
            }
        } log;
        log.paints.reserve(200000);
        window.addComponentListener(&log);
        std::vector<double> vblanks;
        vblanks.reserve(200000);
        VBlankAttachment vblank(&window, [&](double) { vblanks.push_back(Time::getMillisecondCounterHiRes()); });

        struct Event { String kind; std::function<void()> action; double fadeMs; };
        struct Record { String kind; double start, clickMs, cpuMs, systemMs, fadeMs; };
        std::vector<Event> events;
        AVDexedPromptOverlay* prompt = nullptr;
        using View = DexedAudioProcessorEditor::AVView;
        for (int round = 0; round < 3; ++round)
            for (auto view : { View::advanced, View::layers, View::play, View::layers, View::advanced, View::play })
                events.push_back({ "tab change", [editor, view] { editor->showAVView(view, true); }, 220.0 });
        for (int round = 0; round < 4; ++round) {
            events.push_back({ "Settings open", [editor] { editor->showAVSettingsPage(AVDexedSettingsPanel::AVPage::instrument); }, 100.0 });
            events.push_back({ "Settings close", [editor] { editor->AVCloseSettings(false); }, 133.0 });
        }
        for (int round = 0; round < 4; ++round) {
            events.push_back({ "dialog open", [editor, &prompt] {
                AVDexedPromptOverlay::Options options;
                options.title = "Fade test";
                options.message = "A dialog over a blurred copy of the editor.";
                options.confirmText = "OK";
                prompt = editor->showAVPrompt(std::move(options), {});
            }, 83.0 });
            events.push_back({ "dialog close", [&prompt] { if (prompt != nullptr) prompt->dismiss(); prompt = nullptr; }, 117.0 });
        }

        struct Pass { float zoom; bool busy; };
        const Pass passes[] { { 1.0f, false }, { 1.0f, true }, { 0.83f, false }, { 0.83f, true } };
        std::vector<std::pair<String, std::vector<Record>>> results;
        size_t passIndex = 0, eventIndex = 0;
        std::vector<Record> records;
        std::function<void()> runEvent, startPass;
        const auto effects = [&](bool on) {
            for (auto [id, value] : { std::pair { IDs::reverbAmount.parameter(), 0.3f }, { IDs::chorusAmount.parameter(), 0.5f },
                                      { IDs::delayAmount.parameter(), 0.2f } })
                if (auto* parameter = processor.parameters.getParameter(id.getParamID()))
                    parameter->setValueNotifyingHost(on ? value : 0.0f);
        };
        runEvent = [&] {
            if (eventIndex == events.size()) {
                const auto& pass = passes[passIndex];
                results.emplace_back("zoom " + String(pass.zoom, 2) + (pass.busy ? ", 8 layers, effects, 16 keys" : ", silent"), records);
                records.clear();
                ++passIndex;
                if (passIndex == std::size(passes)) { MessageManager::getInstance()->stopDispatchLoop(); return; }
                startPass();
                return;
            }
            const auto& event = events[eventIndex++];
            const double cpu0 = threadSeconds(), system0 = threadSystemSeconds(), t0 = Time::getMillisecondCounterHiRes();
            event.action();
            const double clickMs = Time::getMillisecondCounterHiRes() - t0;
            const double fadeMs = event.fadeMs;
            const String kind = event.kind;
            Timer::callAfterDelay(int(fadeMs) + 120, [&, cpu0, system0, t0, clickMs, fadeMs, kind] {
                records.push_back({ kind, t0, clickMs, (threadSeconds() - cpu0) * 1000.0, (threadSystemSeconds() - system0) * 1000.0, fadeMs });
                Timer::callAfterDelay(500, runEvent);
            });
        };
        startPass = [&] {
            const auto& pass = passes[passIndex];
            playSixteenKeys(processor, false);
            processor.setZoomFactor(pass.zoom);
            editor->resetSize();
            if (pass.busy) {
                processor.stackAVLayers(8, 7.0f);
                effects(true);
            } else {
                processor.resetAVLayers();
                effects(false);
            }
            Timer::callAfterDelay(400, [&] {
                if (passes[passIndex].busy) playSixteenKeys(processor, true);
                eventIndex = 0;
                Timer::callAfterDelay(1500, runEvent);
            });
        };
        Timer::callAfterDelay(800, startPass);
        MessageManager::getInstance()->runDispatchLoop();
        playSixteenKeys(processor, false);
        window.removeComponentListener(&log);

        // The screen's frame period, from its own frames.
        std::vector<double> periods;
        for (size_t i = 1; i < vblanks.size(); ++i) periods.push_back(vblanks[i] - vblanks[i - 1]);
        std::sort(periods.begin(), periods.end());
        const double period = periods.empty() ? 16.7 : periods[periods.size() / 2];
        std::cout << "Screen frame: " << String(period, 2) << " ms (" << String(1000.0 / period, 1) << " Hz)\n";
        const auto frameOf = [&](double t) { return int(std::upper_bound(vblanks.begin(), vblanks.end(), t) - vblanks.begin()); };
        const auto median = [](std::vector<double> v) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
        const auto largest = [](const std::vector<double>& v) { return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()); };
        for (const auto& [passName, passRecords] : results) {
            std::cout << passName << "\n";
            for (const String kind : { "tab change", "Settings open", "Settings close", "dialog open", "dialog close" }) {
                std::vector<double> click, first, painted, gap, cost, worstCost, cpu, system;
                for (const auto& r : passRecords) {
                    if (r.kind != kind) continue;
                    const double end = r.start + r.clickMs + r.fadeMs + 50.0;
                    std::map<int, double> frames;   // screen frame -> paint time in it
                    double firstPaint = -1.0;
                    for (const auto& [at, duration] : log.paints)
                        if (at >= r.start && at <= end) {
                            frames[frameOf(at)] += duration;
                            if (firstPaint < 0.0) firstPaint = at - r.start;
                        }
                    int worst = 0, previous = -1;
                    double sum = 0.0, most = 0.0;
                    for (const auto& [frame, ms] : frames) {
                        if (previous >= 0) worst = jmax(worst, frame - previous);
                        previous = frame;
                        sum += ms;
                        most = jmax(most, ms);
                    }
                    click.push_back(r.clickMs);
                    first.push_back(firstPaint);
                    painted.push_back(double(frames.size()));
                    gap.push_back(double(worst));
                    cost.push_back(frames.empty() ? 0.0 : sum / double(frames.size()));
                    worstCost.push_back(most);
                    cpu.push_back(r.cpuMs);
                    system.push_back(r.systemMs);
                }
                if (click.empty()) continue;
                const double fadeFrames = (kind == "tab change" ? 220.0 : kind == "Settings open" ? 100.0 : kind == "Settings close" ? 133.0
                                           : kind == "dialog open" ? 83.0 : 117.0) / period;
                std::cout << "  " << kind.paddedRight(' ', 15) << " click " << String(median(click), 1) << " ms (max " << String(largest(click), 1)
                          << "), first frame " << String(median(first), 1) << " ms, painted " << roundToInt(median(painted)) << " of ~"
                          << roundToInt(fadeFrames) << " frames, longest gap " << roundToInt(median(gap)) << " (max " << roundToInt(largest(gap))
                          << ") frames, frame paint " << String(median(cost), 2) << " ms (worst " << String(largest(worstCost), 1)
                          << "), message thread " << String(median(cpu), 1) << " ms per fade (kernel " << String(median(system), 1) << ")\n";
            }
        }
        result = 0;
        window.setVisible(false);
        window.clearContentComponent();
    }
    return result;
}

/** --ui-fades-check <bank.syx> <slot> <folder>: the tab change's cross-fade on
    screen, held still: at its first frame it must look like the page before
    the click; half way, the window's compositor (Core Animation on a Mac) must
    show what JUCE paints for the same frame; after it, nothing of it may stay
    (the window must equal a full repaint). At zoom 1.0 and 0.83. The captures
    go to <folder>. Captures are compared with captures, so the screen's colour
    handling is the same on both sides. */
int AVRunUIFadesCheck(const juce::File& bankFile, int slot, const juce::File& outputDirectory) {
    using namespace juce;
    outputDirectory.createDirectory();
    IsolatedLibrary library;
    bool passed = true;
    {
        DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
        if (!loadSound(processor, bankFile, slot)) return 2;
        processor.advancedNoticeHidden = true;
        DocumentWindow window("AV Dexed UI fades check", Colours::black, DocumentWindow::closeButton);
        auto* editor = new DexedAudioProcessorEditor(&processor);
        window.setUsingNativeTitleBar(true);
        window.setContentOwned(editor, true);
        window.setTopLeftPosition(80, 80);
        window.setVisible(true);
        using View = DexedAudioProcessorEditor::AVView;
        editor->showAVView(View::play, false);

        // Pixels that differ by more than `tolerance` levels and the largest
        // difference: over the pages only (the fade's area; the tabs above it
        // change state at the click), or the whole window but the footer (its
        // CPU figure changes by itself).
        const auto compare = [&](const String& what, const Image& a, const Image& b, int tolerance, bool pagesOnly) {
            if (a.isNull() || b.isNull() || a.getBounds() != b.getBounds()) {
                std::cout << "UI fades check: " << what << ": no capture (screen recording permission?)\n";
                passed = false;
                return;
            }
            const float pixelsPerPoint = float(a.getWidth()) / float(editor->getWidth());
            const int titleBar = a.getHeight() - roundToInt(editor->getHeight() * pixelsPerPoint);
            const float footerTop = float(editor->getHeight() - 30.0f * processor.getZoomFactor()) * pixelsPerPoint + float(titleBar);
            // Below the title bar (its look follows the window's focus, which a
            // modal dialog takes) and above the footer.
            auto checked = Rectangle<int>(0, titleBar, a.getWidth(), roundToInt(footerTop) - titleBar);
            if (pagesOnly) {
                const auto pages = editor->getLocalArea(&editor->frameComponent, editor->AVPageArea().toFloat()) * pixelsPerPoint;
                checked = pages.translated(0.0f, float(titleBar)).getSmallestIntegerContainer().getIntersection(checked);
            }
            int over = 0, largest = 0;
            Rectangle<int> area;
            for (int y = checked.getY(); y < checked.getBottom(); ++y)
                for (int x = checked.getX(); x < checked.getRight(); ++x) {
                    const auto p = a.getPixelAt(x, y), q = b.getPixelAt(x, y);
                    const int change = jmax(std::abs(p.getRed() - q.getRed()), std::abs(p.getGreen() - q.getGreen()),
                                            std::abs(p.getBlue() - q.getBlue()));
                    largest = jmax(largest, change);
                    if (change > tolerance) {
                        ++over;
                        area = area.isEmpty() ? Rectangle<int>(x, y, 1, 1) : area.getUnion({ x, y, 1, 1 });
                    }
                }
            const bool ok = over == 0;
            passed = passed && ok;
            std::cout << "UI fades check, zoom " << String(processor.getZoomFactor(), 2) << ": " << what << ": " << over
                      << " px differ by more than " << tolerance << " levels" << (over > 0 ? " in " + area.toString() : String())
                      << " (largest " << largest << ")" << (ok ? "" : "  FAILED") << "\n";
        };
        const auto file = [&](const String& name) {
            return outputDirectory.getChildFile("fade-" + String(processor.getZoomFactor(), 2) + "-" + name + ".png");
        };
        const auto still = [&](float at, bool native) {
            editor->AVPageFade.finish();
            editor->showAVView(View::play, false);
            AVDexedCrossfade::testFreeze = at;
            AVDexedNativeFade::enabled = native;
            editor->showAVView(View::advanced, true);
        };
        const auto end = [&] {
            editor->AVPageFade.finish();
            AVDexedCrossfade::testFreeze = -1.0f;
            AVDexedNativeFade::enabled = true;
        };

        // The checks run as stages, each after its delay (ms).
        std::map<String, Image> shots;
        const auto shoot = [&](const String& name) { shots[name] = captureWindow(file(name)); };
        std::vector<std::pair<int, std::function<void()>>> stages;
        const auto add = [&](int ms, std::function<void()> stage) { stages.emplace_back(ms, std::move(stage)); };

        // In-editor dialogs: Settings and a prompt, faded in and out by AVDexedComponentFade.
        struct Overlay {
            String name;
            std::function<void()> open, close, reset;
            std::function<Component*()> target;
        };
        AVDexedPromptOverlay* prompt = nullptr;
        const Overlay overlays[] {
            { "settings",
              [&] { editor->showAVSettingsPage(AVDexedSettingsPanel::AVPage::instrument); },
              [&] { editor->AVCloseSettings(false); },
              [&] {
                  AVDexedComponentFade::testFreeze = -1.0f;
                  if (editor->AVSettingsOverlay) AVDexedComponentFade::cancel(*editor->AVSettingsOverlay);
                  editor->AVCloseSettings(true);
              },
              [&]() -> Component* { return editor->AVSettingsOverlay.get(); } },
            { "dialog",
              [&] {
                  AVDexedPromptOverlay::Options options;
                  options.title = "Fade check";
                  options.message = "A dialog over a blurred copy of the editor.";
                  options.confirmText = "OK";
                  prompt = editor->showAVPrompt(std::move(options), {});
              },
              [&] { if (prompt != nullptr) prompt->dismiss(); },
              [&] {
                  AVDexedComponentFade::testFreeze = -1.0f;
                  if (prompt != nullptr) {
                      AVDexedComponentFade::cancel(*prompt);
                      prompt->dismiss();
                      AVDexedComponentFade::cancel(*prompt);
                  }
                  prompt = nullptr;
              },
              [&]() -> Component* { return prompt; } },
        };
        const auto held = [&](const Overlay& overlay, float at, bool native, bool opening) {
            AVDexedNativeFade::enabled = native;
            AVDexedComponentFade::testFreeze = at;
            if (opening) overlay.open(); else overlay.close();
        };

        for (float zoom : { 1.0f, 0.83f }) {
            add(0, [&, zoom] {
                processor.setZoomFactor(zoom);
                editor->resetSize();
                editor->showAVView(View::play, false);
            });
            add(1200, [&] { shoot("before"); still(0.0f, true); });
            add(500, [&] { shoot("start-native"); still(0.0f, false); });
            add(500, [&] { shoot("start-painted"); still(0.5f, true); });
            add(500, [&] { shoot("half-native"); still(0.5f, false); });
            add(500, [&] {
                shoot("half-painted");
                end();
                editor->showAVView(View::play, false);
            });
            add(400, [&] { editor->showAVView(View::advanced, true); });   // a real fade, to its end
            add(900, [&] { shoot("after"); editor->repaint(); });
            add(600, [&] {
                shoot("after-repaint");
                // Up to 3 levels: CoreGraphics dithers gradients, so two paints differ.
                compare("tabs, first frame (Core Animation) = page before", shots["start-native"], shots["before"], 3, true);
                compare("tabs, first frame (painted) = page before", shots["start-painted"], shots["before"], 3, true);
                // The compositor mixes the pictures in linear light, CoreGraphics
                // in sRGB values: half way, high-contrast edges come out up to
                // ~13 levels lighter. The ends are exact (above and below).
                compare("tabs, half way, Core Animation ~ painted", shots["half-native"], shots["half-painted"], 16, true);
                compare("tabs, after the fade = full repaint", shots["after"], shots["after-repaint"], 0, false);
                editor->showAVView(View::play, false);
            });
            for (const auto& overlay : overlays) {
                const auto n = [&overlay](const char* what) { return overlay.name + "-" + what; };
                add(700, [&, n] { overlay.reset(); });
                add(700, [&, n] { shoot(n("closed")); held(overlay, 0.0f, true, true); });
                add(500, [&, n] { shoot(n("in-start")); overlay.reset(); });
                add(700, [&, n] { held(overlay, 1.0f, true, true); });
                add(500, [&, n] {
                    shoot(n("in-end"));
                    AVDexedComponentFade::testFreeze = -1.0f;
                    if (auto* target = overlay.target()) AVDexedComponentFade::cancel(*target);   // shown, live
                });
                add(500, [&, n] { shoot(n("open")); overlay.reset(); });
                add(700, [&, n] { held(overlay, 0.5f, true, true); });
                add(500, [&, n] { shoot(n("half-native")); overlay.reset(); });
                add(700, [&, n] { held(overlay, 0.5f, false, true); });
                add(500, [&, n] { shoot(n("half-painted")); overlay.reset(); });
                add(700, [&, n] { AVDexedNativeFade::enabled = true; overlay.open(); });
                add(700, [&, n] { held(overlay, 0.0f, true, false); });
                add(500, [&, n] {
                    shoot(n("out-start"));
                    AVDexedComponentFade::testFreeze = -1.0f;
                    if (auto* target = overlay.target()) AVDexedComponentFade::cancel(*target);   // the picture goes
                });
                add(600, [&, n] {
                    shoot(n("gone"));
                    const auto what = [&](const char* text) { return overlay.name + ", " + text; };
                    compare(what("fade-in's first frame = before"), shots[n("in-start")], shots[n("closed")], 3, false);
                    compare(what("fade-in's last frame = open"), shots[n("in-end")], shots[n("open")], 3, false);
                    compare(what("half way, Core Animation ~ painted"), shots[n("half-native")], shots[n("half-painted")], 16, false);
                    compare(what("fade-out's first frame = open"), shots[n("out-start")], shots[n("open")], 3, false);
                    compare(what("after the fade-out = before"), shots[n("gone")], shots[n("closed")], 3, false);
                    overlay.reset();
                });
            }
        }
        add(300, [] { MessageManager::getInstance()->stopDispatchLoop(); });
        size_t stage = 0;
        std::function<void()> next;
        next = [&] {
            stages[stage].second();
            if (++stage < stages.size()) Timer::callAfterDelay(stages[stage].first, next);
        };
        Timer::callAfterDelay(500, next);
        MessageManager::getInstance()->runDispatchLoop();
        end();
        window.setVisible(false);
        window.clearContentComponent();
    }
    std::cout << (passed ? "UI fades check passed.\n" : "UI fades check FAILED.\n");
    return passed ? 0 : 1;
}

/** --ui-stress <bank.syx> <slot> <minutes>: the editor in a real window under
    a long, busy session: eight layers with effects and an arpeggio sound all
    along while, every 0.25-1.5 s, something happens at random (fixed seed):
    a tab change, Settings or a dialog opened and closed, a zoom change, the
    next sound, the layers stacked or reset, low power or the effects
    switched. Every minute: the process's memory (a leak grows it), the message
    thread's CPU, the longest the message thread went without a screen frame
    (a stall), the audio thread's longest block against its 2.67 ms (128
    samples at 48 kHz) and how many blocks ran over it. At the end, no fade may
    be left on screen. Exit code 0 when it ran to the end with no fade left. */
int AVRunUIStress(const juce::File& bankFile, int slot, int minutes) {
    using namespace juce;
    IsolatedLibrary library;
    int result = 1;
    {
        DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
        if (!loadSound(processor, bankFile, slot)) return 2;
        processor.advancedNoticeHidden = true;
        // The audio thread, paced in real time, timing every block.
        struct StressAudio {
            const double rate = 48000.0;
            const int block = 128;
            explicit StressAudio(DexedAudioProcessor& p) : processor(p) {
                processor.setRateAndBufferSizeDetails(rate, block);
                processor.prepareToPlay(rate, block);
                thread = std::thread([this] {
                    AudioBuffer<float> buffer(2, block);
                    auto next = std::chrono::steady_clock::now();
                    const auto period = std::chrono::nanoseconds(int64(1.0e9 * block / rate));
                    while (running.load()) {
                        const double start = threadSeconds();
                        MidiBuffer midi;
                        buffer.clear();
                        processor.processBlock(buffer, midi);
                        const double ms = (threadSeconds() - start) * 1000.0;
                        if (ms > longestMs.load()) longestMs.store(ms);
                        if (ms > 1000.0 * block / rate) overruns.fetch_add(1);
                        next += period;
                        std::this_thread::sleep_until(next);
                    }
                });
            }
            ~StressAudio() { running.store(false); thread.join(); }
            DexedAudioProcessor& processor;
            std::atomic<bool> running { true };
            std::atomic<double> longestMs { 0.0 };
            std::atomic<int> overruns { 0 };
            std::thread thread;
        } audio(processor);

        DocumentWindow window("AV Dexed UI stress", Colours::black, DocumentWindow::closeButton);
        auto* editor = new DexedAudioProcessorEditor(&processor);
        window.setUsingNativeTitleBar(true);
        window.setContentOwned(editor, true);
        window.setTopLeftPosition(80, 80);
        window.setVisible(true);
        using View = DexedAudioProcessorEditor::AVView;

        double lastFrame = Time::getMillisecondCounterHiRes(), longestStall = 0.0;
        VBlankAttachment vblank(&window, [&](double) {
            const double now = Time::getMillisecondCounterHiRes();
            longestStall = jmax(longestStall, now - lastFrame);
            lastFrame = now;
        });
        const auto residentMB = [] {
#if defined(__APPLE__)
            mach_task_basic_info_data_t info {};
            mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
            if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t) &info, &count) == KERN_SUCCESS)
                return double(info.resident_size) / (1024.0 * 1024.0);
#endif
            return 0.0;
        };
        const auto effects = [&](bool on) {
            for (auto [id, value] : { std::pair { IDs::reverbAmount.parameter(), 0.3f }, { IDs::chorusAmount.parameter(), 0.5f },
                                      { IDs::delayAmount.parameter(), 0.2f } })
                if (auto* parameter = processor.parameters.getParameter(id.getParamID()))
                    parameter->setValueNotifyingHost(on ? value : 0.0f);
        };
        processor.stackAVLayers(8, 7.0f);
        effects(true);
        bool effectsOn = true;

        // The arpeggio: eight notes a second over two octaves, all along.
        bool playing = true;
        int step = 0;
        std::function<void()> arpeggio;
        arpeggio = [&] {
            if (! playing) return;
            static constexpr int pattern[] { 48, 52, 55, 60, 64, 67, 72, 67, 64, 60, 55, 52 };
            const int count = int(std::size(pattern));
            processor.keyboardState.noteOff(1, pattern[(step + count - 1) % count], 0.0f);
            processor.keyboardState.noteOn(1, pattern[step % count], 0.8f);
            ++step;
            Timer::callAfterDelay(125, arpeggio);
        };
        arpeggio();

        Random random(20261021);
        std::map<String, int> done;
        String slowest;
        double slowestMs = 0.0;
        AVDexedPromptOverlay* prompt = nullptr;
        std::function<void()> act;
        const auto timed = [&] {
            const auto before = done;
            const double t0 = Time::getMillisecondCounterHiRes();
            act();
            const double ms = Time::getMillisecondCounterHiRes() - t0;
            if (ms > slowestMs) {
                slowestMs = ms;
                for (const auto& [name, count] : done)
                    if (before.count(name) == 0 || before.at(name) != count) slowest = name;
            }
        };
        // As a person would: what a modal dialog covers cannot be clicked, and a
        // dialog the editor opens by itself (the question about unsaved changes
        // when the sound changes) is answered (cancelled) before anything else.
        act = [&] {
            for (auto& kept : editor->AVPrompts)
                if (! kept->isFinishing() && kept.get() != prompt) {
                    kept->dismiss();
                    ++done["answered"];
                    return;
                }
            const bool modal = ModalComponentManager::getInstance()->getNumModalComponents() > 0;
            const int pick = random.nextInt(100);
            if (pick < 35) {
                if (modal) return;
                const View views[] { View::play, View::advanced, View::layers };
                editor->showAVView(views[random.nextInt(3)], true);
                ++done["tab change"];
            } else if (pick < 47) {
                if (editor->AVSettingsOverlay != nullptr && editor->AVSettingsOverlay->isVisible()) {
                    editor->AVCloseSettings(false);
                } else if (modal) {
                    return;
                } else {
                    const AVDexedSettingsPanel::AVPage pages[] { AVDexedSettingsPanel::AVPage::audio, AVDexedSettingsPanel::AVPage::instrument,
                                                                 AVDexedSettingsPanel::AVPage::midi, AVDexedSettingsPanel::AVPage::about };
                    editor->showAVSettingsPage(pages[random.nextInt(4)]);
                }
                ++done["Settings"];
            } else if (pick < 57) {
                if (prompt != nullptr) {
                    prompt->dismiss();
                    prompt = nullptr;
                } else if (! modal) {
                    AVDexedPromptOverlay::Options options;
                    options.title = "Stress";
                    options.message = "A dialog opened and closed at random.";
                    options.confirmText = "OK";
                    prompt = editor->showAVPrompt(std::move(options), {});
                }
                ++done["dialog"];
            } else if (pick < 64) {
                if (modal) return;
                const float zooms[] { 0.8f, 0.9f, 1.0f, 1.1f, 1.25f };
                processor.setZoomFactor(zooms[random.nextInt(5)]);
                editor->resetSize();
                ++done["zoom"];
            } else if (pick < 80) {
                if (! editor->AVCanStepSounds()) return;
                editor->playView.navigatePreset(random.nextBool() ? 1 : -1);
                ++done["sound"];
            } else if (pick < 86) {
                if (processor.isAVLayerModeOn()) processor.resetAVLayers();
                else processor.stackAVLayers(8, 7.0f);
                ++done["layers"];
            } else if (pick < 92) {
                AVDexedTheme::lowPower = ! AVDexedTheme::lowPower;
                editor->repaint();
                ++done["low power"];
            } else {
                effectsOn = ! effectsOn;
                effects(effectsOn);
                ++done["effects"];
            }
        };

        const double startMs = Time::getMillisecondCounterHiRes();
        double minuteStart = startMs, minuteCpu = threadSeconds(), firstMB = residentMB();
        int minute = 0;
        std::cout << "UI stress: " << minutes << " min, memory at the start " << String(firstMB, 1) << " MB\n" << std::flush;
        std::function<void()> tick;
        tick = [&] {
            const double now = Time::getMillisecondCounterHiRes();
            if (now - minuteStart >= 60000.0) {
                ++minute;
                const double cpu = (threadSeconds() - minuteCpu) / ((now - minuteStart) / 1000.0) * 100.0;
                String actions;
                for (const auto& [name, count] : done) actions << name << " " << count << ", ";
                std::function<int(Component&)> descendants = [&](Component& c) {
                    int n = 1;
                    for (auto* child : c.getChildren()) n += descendants(*child);
                    return n;
                };
                std::cout << "  min " << minute << ": components " << descendants(window) << ", modal "
                          << ModalComponentManager::getInstance()->getNumModalComponents() << ", desktop "
                          << Desktop::getInstance().getNumComponents() << ", prompts kept " << editor->AVPrompts.size()
                          << ", memory " << String(residentMB(), 1) << " MB, message thread "
                          << String(cpu, 1) << " %, longest stall " << String(longestStall, 1) << " ms, audio longest block "
                          << String(audio.longestMs.load(), 2) << " ms (" << audio.overruns.load() << " over 2.67 ms), slowest action "
                          << slowest << " " << String(slowestMs, 1) << " ms; " << actions.dropLastCharacters(2) << "\n" << std::flush;
                done.clear();
                slowest.clear();
                slowestMs = 0.0;
                longestStall = 0.0;
                audio.longestMs.store(0.0);
                audio.overruns.store(0);
                minuteStart = now;
                minuteCpu = threadSeconds();
                if (minute >= minutes) {
                    playing = false;
                    processor.keyboardState.allNotesOff(1);
                    if (prompt != nullptr) prompt->dismiss();
                    prompt = nullptr;
                    editor->AVCloseSettings(false);
                    Timer::callAfterDelay(1500, [&] { MessageManager::getInstance()->stopDispatchLoop(); });
                    return;
                }
            }
            timed();
            Timer::callAfterDelay(250 + random.nextInt(1250), tick);
        };
        Timer::callAfterDelay(1500, tick);
        MessageManager::getInstance()->runDispatchLoop();

        // Nothing of a fade may stay on screen.
        int covers = 0;
        for (auto* child : editor->frameComponent.getChildren())
            if (dynamic_cast<AVDexedComponentFade*>(child) != nullptr) ++covers;
        const bool clean = covers == 0 && ! editor->AVPageFade.isRunning();
        std::cout << "UI stress: memory at the end " << String(residentMB(), 1) << " MB (" << String(residentMB() - firstMB, 1)
                  << " MB more than at the start), " << covers << " fade covers left, page fade "
                  << (editor->AVPageFade.isRunning() ? "still running" : "ended") << (clean ? "" : "  FAILED") << "\n";
        result = clean ? 0 : 1;
        AVDexedTheme::lowPower = false;
        window.setVisible(false);
        window.clearContentComponent();
    }
    return result;
}

/** --ui-sounds <bank.syx> <slot>: the next sound 60 times (every 300 ms, Play
    page, a real window, eight layers stacked first), as the arrow beside the
    sound's name does: how long each change takes on the message thread, and
    the first paint after it. */
int AVRunUISounds(const juce::File& bankFile, int slot) {
    using namespace juce;
    IsolatedLibrary library;
    {
        DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
        if (!loadSound(processor, bankFile, slot)) return 2;
        processor.advancedNoticeHidden = true;
        RealTimeAudio audio(processor);
        DocumentWindow window("AV Dexed UI sounds", Colours::black, DocumentWindow::closeButton);
        auto* editor = new DexedAudioProcessorEditor(&processor);
        window.setUsingNativeTitleBar(true);
        window.setContentOwned(editor, true);
        window.setTopLeftPosition(80, 80);
        window.setVisible(true);
        editor->showAVView(DexedAudioProcessorEditor::AVView::play, false);
        processor.stackAVLayers(8, 7.0f);
        std::vector<double> changes, paints;
        struct FirstPaint : ComponentListener {
            double since = -1.0, took = -1.0;
            void componentPainted(Component&, const ComponentPaintDiagnostics&) override {
                if (since >= 0.0 && took < 0.0) took = Time::getMillisecondCounterHiRes() - since;
            }
        } first;
        window.addComponentListener(&first);
        int left = 60;
        std::function<void()> step;
        step = [&] {
            if (first.took >= 0.0) paints.push_back(first.took);
            if (left-- == 0) { MessageManager::getInstance()->stopDispatchLoop(); return; }
            first.took = -1.0;
            first.since = Time::getMillisecondCounterHiRes();
            editor->playView.navigatePreset(1);
            changes.push_back(Time::getMillisecondCounterHiRes() - first.since);
            Timer::callAfterDelay(300, step);
        };
        Timer::callAfterDelay(1500, step);
        MessageManager::getInstance()->runDispatchLoop();
        window.removeComponentListener(&first);
        const auto summary = [](std::vector<double> v) {
            if (v.empty()) return String("none");
            std::sort(v.begin(), v.end());
            return "median " + String(v[v.size() / 2], 1) + " ms, max " + String(v.back(), 1) + " ms";
        };
        std::cout << "UI sounds: change " << summary(changes) << "; until the first paint " << summary(paints) << "\n";
        window.setVisible(false);
        window.clearContentComponent();
    }
    return 0;
}
