// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// --ui-cpu <bank.syx> <slot>: how much CPU the editor and the audio take in a
// real window, as when playing. A thread renders audio in real time (128
// samples at 48 kHz) while the editor shows each page; the process CPU minus
// the audio thread's is what the editor costs (the window server composites
// on top of that, in its own process). The message thread's own share (where
// the editor paints) is printed apart.
//
// --ui-resize <bank.syx> <slot> <folder>: the window is resized as the grip
// does (zoom 1.0 → 0.8 → 1.6 → 1.25) while eight layers sound, then the keys
// go up and everything settles. The window as the screen shows it then must
// equal the same window after a full repaint: a region that was not redrawn
// (or drawn black) after the resize would differ. Then the same window is
// painted with no cached images or text (AVDexedPixelCache and
// AVDexedTheme::TextCache off): it must look the same. The captures go to <folder>.

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <unistd.h>
#endif
#include "../source/ui/PluginEditor.h"
#include "../source/ui/AVDexedPixelCache.h"
#include "../source/core/AVDexedLibrary.h"
#include "../source/parameter/Model.h"
#include <atomic>
#include <chrono>
#include <ctime>
#include <iostream>
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
            { "window hidden, 8 layers, effects", [&] {
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
