// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../PluginProcessor.h"
#include "AVDexedSettingsStyle.h"
#include <array>
#include <functional>
#include <memory>

/** The "Capas" page: the layer mode switch and eight strips, one per layer,
    as the eight modules of a rack (X816 mode). Each strip turns its layer on or off,
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

    /** Plays an old layers file (.avdexlayers, 0.3.2 and before). */
    void loadLayersFile(const juce::File& file);
    /** The layers now are a layers preset's, or an old layers file's. */
    void showLayerSet();

    /** A short line for the footer (what a button did). */
    std::function<void(const juce::String&)> onNotice;
    /** Runs what replaces the sound being edited (a layers file) once the
        editor has asked about unsaved changes. */
    std::function<void(std::function<void()>)> onReplaceSound;

    class Strip;

private:
    friend bool AVRenderUIEvidence(const juce::File& directory);
    DexedAudioProcessor& processor;
    juce::ToggleButton modeToggle;
    AVDexedSettingsStyle::OnOffPills modePills { modeToggle };
    // Opening and saving are the editor's: a drop or "Importar", and the Save button.
    juce::TextButton stackButton, resetButton;
    void installLayersFile(const juce::File& file, const juce::String& text);
    std::array<std::unique_ptr<Strip>, DexedAudioProcessor::AVLayerCount> strips;
    juce::Rectangle<float> modeLabelArea, hintArea;
    bool shownMode = false;

    void timerCallback() override;
    void refresh();
};
