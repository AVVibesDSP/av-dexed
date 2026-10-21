// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// --ui-cpu <bank.syx> <slot>: how much CPU the editor and the audio take in a
// real window, as when playing. A thread renders audio in real time (128
// samples at 48 kHz) while the editor shows each page; the process CPU minus
// the audio thread's is what the editor costs (the window server composites
// on top of that, in its own process).

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "../source/ui/PluginEditor.h"
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
}

int AVRunUICpu(const juce::File& bankFile, int slot) {
    using namespace juce;
    // An isolated library, as --render-ui: the user's folders are never read or written.
    const auto oldAppDirectory = DexedAudioProcessor::dexedAppDir;
    const auto oldCartDirectory = DexedAudioProcessor::dexedCartDir;
    const auto libraryRoot = File::getSpecialLocation(File::tempDirectory).getChildFile("avdexed-uicpu-" + Uuid().toString());
    libraryRoot.createDirectory();
    DexedAudioProcessor::dexedAppDir = libraryRoot;
    DexedAudioProcessor::dexedCartDir = AVDexedLibrary::rootFor(libraryRoot);
    AVDexedLibrary::installFactorySounds(DexedAudioProcessor::dexedCartDir, BinaryData::builtin_pgm_zip,
                                         (size_t) BinaryData::builtin_pgm_zipSize, false);
    int result = 0;
    {
        DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
        Cartridge bank;
        if (bank.load(bankFile) < 0) return 2;
        processor.setActiveBank(bank, bankFile);
        processor.setCurrentProgram(jlimit(0, 31, slot - 1));
        constexpr double rate = 48000.0;
        constexpr int block = 128;
        processor.setRateAndBufferSizeDetails(rate, block);
        processor.prepareToPlay(rate, block);

        // The audio thread, paced in real time.
        std::atomic<bool> running { true };
        std::atomic<double> audioSeconds { 0.0 };
        std::thread audio([&] {
            AudioBuffer<float> buffer(2, block);
            auto next = std::chrono::steady_clock::now();
            const auto period = std::chrono::nanoseconds(int64(1.0e9 * block / rate));
            while (running.load()) {
                const double start = threadSeconds();
                MidiBuffer midi;
                buffer.clear();
                processor.processBlock(buffer, midi);
                audioSeconds.store(audioSeconds.load() + threadSeconds() - start);
                next += period;
                std::this_thread::sleep_until(next);
            }
        });

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
        // Sixteen keys held: with eight layers every voice of every layer sounds.
        const auto play = [&](bool on) {
            for (int n = 0; n < 16; ++n) {
                if (on) processor.keyboardState.noteOn(1, 36 + n * 3, 0.8f);
                else processor.keyboardState.noteOff(1, 36 + n * 3, 0.0f);
            }
        };
        const auto effects = [&](bool on) {
            for (auto [id, value] : { std::pair { IDs::reverbAmount.parameter(), 0.3f }, { IDs::chorusAmount.parameter(), 0.5f },
                                      { IDs::delayAmount.parameter(), 0.2f } })
                if (auto* parameter = processor.parameters.getParameter(id.getParamID()))
                    parameter->setValueNotifyingHost(on ? value : 0.0f);
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
            { "window hidden, 8 layers, 16 keys, effects", [&] { window.setVisible(false); } },
        };
        constexpr double settle = 1.5, measure = 5.0;
        size_t index = 0;
        double phaseStart = 0.0, cpuStart = 0.0, audioStart = 0.0;
        bool measuring = false, finished = false;
        std::function<void()> tick;
        tick = [&] {
            const double now = Time::getMillisecondCounterHiRes() / 1000.0;
            if (! measuring && now - phaseStart >= settle) {
                measuring = true;
                phaseStart = now;
                cpuStart = processSeconds();
                audioStart = audioSeconds.load();
            } else if (measuring && now - phaseStart >= measure) {
                const double total = (processSeconds() - cpuStart) / measure * 100.0;
                const double audioPart = (audioSeconds.load() - audioStart) / measure * 100.0;
                std::cout << String(steps[index].name).paddedRight(' ', 40) << " process " << String(total, 1).paddedLeft(' ', 5)
                          << " %   audio " << String(audioPart, 1).paddedLeft(' ', 5) << " %   rest (editor) "
                          << String(total - audioPart, 1).paddedLeft(' ', 5) << " %  of one core, "
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
        running.store(false);
        audio.join();
        window.setVisible(false);
        editor->removeChildComponent(&counter);
        window.clearContentComponent();
    }
    DexedAudioProcessor::dexedAppDir = oldAppDirectory;
    DexedAudioProcessor::dexedCartDir = oldCartDirectory;
    if (libraryRoot.isAChildOf(File::getSpecialLocation(File::tempDirectory)) && libraryRoot.getFileName().startsWith("avdexed-uicpu-"))
        libraryRoot.deleteRecursively();
    return result;
}
