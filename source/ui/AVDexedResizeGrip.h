// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class AVDexedResizeGrip final : public juce::Component, public juce::SettableTooltipClient {
public:
    AVDexedResizeGrip();

    std::function<void()> onResizeStart;
    std::function<void(juce::Point<int>)> onResizeDrag;
    std::function<void()> onResizeEnd;

    void paint(juce::Graphics& g) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    std::unique_ptr<juce::Drawable> icon;
    juce::Point<int> dragStartScreen;
    bool hovered = false;
    bool dragging = false;
};
