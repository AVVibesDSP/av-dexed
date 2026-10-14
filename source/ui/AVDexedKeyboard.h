// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

/** Vector piano that keeps JUCE's MidiKeyboardState note tracking and input
    behaviour while using the AV Dexed key treatment. */
class AVDexedKeyboardComponent final : public juce::MidiKeyboardComponent {
public:
    explicit AVDexedKeyboardComponent(juce::MidiKeyboardState& state);
    void releaseAVUINotes();
    void timerCallback() override;
    bool mouseDownOnKey(int, const juce::MouseEvent&) override;
    void mouseUpOnKey(int, const juce::MouseEvent&) override;

    void drawWhiteNote(int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area,
                       bool isDown, bool isOver, juce::Colour lineColour,
                       juce::Colour textColour) override;
    void drawBlackNote(int midiNoteNumber, juce::Graphics& g, juce::Rectangle<float> area,
                       bool isDown, bool isOver, juce::Colour noteFillColour) override;
private:
    bool AVMouseHeld = false;
};
