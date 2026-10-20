// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../PluginProcessor.h"
#include "AVDexedSettingsStyle.h"
#include <array>
#include <functional>
#include <memory>

/** The "Capas" page: the layer mode switch and eight strips, one per layer,
    as the eight modules of a TX816. Each strip turns its layer on or off,
    shows its sound (a click selects it: Play, Advanced and the browser then
    work on that layer) and sets its volume, pan, tuning, transposition, MIDI
    channel and keys. Same vocabulary as Advanced and Settings: the 12% boxes,
    ocean letters for "on" and for the selected layer, knobs with the value
    under them, lists where GOZO uses lists. */
class AVDexedLayersView final : public juce::Component, private juce::Timer {
public:
    explicit AVDexedLayersView(DexedAudioProcessor& processor);
    ~AVDexedLayersView() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void applyLanguage();
    void visibilityChanged() override;

    /** A short line for the footer (what a button did). */
    std::function<void(const juce::String&)> onNotice;

    class Strip;

private:
    DexedAudioProcessor& processor;
    juce::ToggleButton modeToggle;
    AVDexedSettingsStyle::OnOffPills modePills { modeToggle };
    juce::TextButton stackButton, resetButton;
    std::array<std::unique_ptr<Strip>, DexedAudioProcessor::AVLayerCount> strips;
    juce::Rectangle<float> modeLabelArea, hintArea;
    bool shownMode = false;

    void timerCallback() override;
    void refresh();
};
