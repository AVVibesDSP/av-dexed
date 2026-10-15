// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ParamDialog.h"
#include "../AVDexedSettingsStyle.h"

class DexedAudioProcessor;

/** Product-scoped settings view. The standalone device manager is supplied by
    the application; plugin instances continue to use the host audio/MIDI bus. */
class AVDexedSettingsPanel final : public juce::Component, private juce::ChangeListener {
public:
    struct AVCallbacks {
        std::function<void()> openAudioSettings;
        std::function<std::unique_ptr<juce::Component>()> createAudioContent;
        std::function<std::unique_ptr<juce::Component>()> createMidiContent;
        std::function<void(ParamDialog*)> applyInstrumentSettings;
        std::function<void(ParamDialog*, ParamDialog::TuningAction)> tuningAction;
        std::function<void()> close;
    };
    enum class AVPage { audio, instrument, midi, sysex, about };

    AVDexedSettingsPanel(DexedAudioProcessor&, AVCallbacks);
    ~AVDexedSettingsPanel() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void AVShowPage(AVPage);
    AVPage AVGetPage() const noexcept { return AVSelectedPage; }
    ParamDialog& AVGetInstrumentSettings() { return *AVInstrument; }
private:
    class AVTabButton;
    class AVAudioPage;
    class AVMidiPage;
    class AVSysExPage;
    class AVAboutPage;
    DexedAudioProcessor& AVProcessor;
    AVCallbacks AVActions;
    AVDexedSettingsStyle::LookAndFeel AVLookAndFeel;
    std::array<std::unique_ptr<AVTabButton>, 5> AVTabs;
    juce::DrawableButton AVClose { "close", juce::DrawableButton::ImageFitted };
    std::unique_ptr<ParamDialog> AVInstrument;
    std::unique_ptr<AVAudioPage> AVAudio;
    std::unique_ptr<juce::Component> AVMidi;
    std::unique_ptr<AVAboutPage> AVAbout;
    std::unique_ptr<AVSysExPage> AVSysEx;
    juce::Label AVMidiHostNotice;
    AVPage AVSelectedPage = AVPage::audio;
    void AVRefreshLanguage();
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AVDexedSettingsPanel)
};
