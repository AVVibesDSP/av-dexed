// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "DXLookNFeel.h"
#include "AVDexedTheme.h"

/** GOZO's settings vocabulary, shared by every Settings page (Audio, the
    Instrument page, MIDI, SysEx, About):
    - labels Regular 15 in a right-aligned column 0..190, fields from x = 202,
      rows 30 tall every 36;
    - a value as plain text (46 wide) followed by a thin fader 12 px later;
    - pills (the 12% box, radius 9) whose letters turn ocean when "on";
    - two-pill On/Off choices and ring toggles instead of switches;
    - section headings SemiBold 18 over a hairline, hints Regular 13 at 70%. */
namespace AVDexedSettingsStyle {
inline constexpr float labelWidth = 190.0f, fieldX = 202.0f, rowHeight = 30.0f, rowGap = 36.0f;
inline constexpr float bodySize = 15.0f, hintSize = 13.0f, valueWidth = 46.0f, valueGap = 12.0f;

inline juce::Font body() { return AVDexedTheme::font(bodySize); }

inline void label(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                  juce::Justification justification = juce::Justification::centredRight) {
    AVDexedTheme::drawGreyText(g, text, body(), area, justification);
}

inline void hint(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                 juce::Justification justification = juce::Justification::centredLeft) {
    AVDexedTheme::drawGreyText(g, text, AVDexedTheme::font(hintSize), area, justification, 0.7f);
}

/** Multi-line hint (wraps inside `area`). */
inline void hintParagraph(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area) {
    juce::AttributedString string;
    string.setLineSpacing(3.0f);
    string.append(text, AVDexedTheme::font(hintSize), AVDexedTheme::textPrimary.interpolatedWith(AVDexedTheme::textSecondary, 0.4f).withAlpha(0.7f));
    juce::TextLayout layout;
    layout.createLayout(string, area.getWidth());
    layout.draw(g, area);
}

inline void heading(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area) {
    AVDexedTheme::drawGreyText(g, text, AVDexedTheme::font(18.0f, "SemiBold"), area, juce::Justification::centredLeft);
    const auto rule = area.withTop(area.getBottom() - 1.0f).withHeight(1.0f);
    juce::ColourGradient grey(AVDexedTheme::textPrimary, 0.0f, rule.getY(), AVDexedTheme::textSecondary, 0.0f, rule.getBottom(), false);
    grey.multiplyOpacity(0.35f);
    g.setGradientFill(grey);
    g.fillRect(rule);
}

/** Width a pill needs for its text at the body size. */
inline int pillWidth(const juce::String& text, int minimum = 60) {
    return juce::jmax(minimum, juce::roundToInt(juce::GlyphArrangement::getStringWidth(body(), text)) + 26);
}

/** Look of every control inside Settings. */
class LookAndFeel final : public DXLookNFeel {
public:
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return body(); }
    juce::Font getComboBoxFont(juce::ComboBox&) override { return body(); }

    /** The value reads as plain text, like GOZO's "120" before its fader. */
    juce::Label* createSliderTextBox(juce::Slider& slider) override {
        auto* box = DXLookNFeel::createSliderTextBox(slider);
        box->setFont(body());
        box->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        box->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
        box->setJustificationType(juce::Justification::centredLeft);
        box->setBorderSize(juce::BorderSize<int>(0));
        return box;
    }
    juce::Slider::SliderLayout getSliderLayout(juce::Slider& slider) override {
        if (slider.getSliderStyle() != juce::Slider::LinearHorizontal || slider.getTextBoxPosition() != juce::Slider::TextBoxLeft)
            return DXLookNFeel::getSliderLayout(slider);
        juce::Slider::SliderLayout layout;
        auto area = slider.getLocalBounds();
        layout.textBoxBounds = area.removeFromLeft(int(valueWidth));
        area.removeFromLeft(int(valueGap));
        // The value maps over sliderBounds and the track is drawn over the same
        // span; the inset keeps the thumb whole at both ends (0 dB is the
        // general volume's resting place, its maximum).
        layout.sliderBounds = area.reduced(AVFaderInset, 0);
        return layout;
    }

    /** Toggles marked "AVRing" are GOZO's ring + dot (the MIDI inputs). */
    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool hover, bool down) override {
        if (!bool(button.getProperties()["AVRing"])) {
            DXLookNFeel::drawToggleButton(g, button, hover, down);
            return;
        }
        const bool on = button.getToggleState();
        const float alpha = button.isEnabled() ? 1.0f : 0.42f;
        const auto ring = juce::Rectangle<float>(16.0f, 16.0f).withCentre(button.getLocalBounds().toFloat().getCentre());
        g.setColour(AVDexedTheme::oceanHighlight.withAlpha((on ? 1.0f : hover ? 0.75f : 0.45f) * alpha));
        g.drawEllipse(ring.reduced(0.75f), 1.5f);
        if (on) {
            const auto dot = ring.reduced(4.5f);
            g.setGradientFill(juce::ColourGradient(AVDexedTheme::oceanHighlight.withAlpha(alpha), 0.0f, dot.getY(),
                                                   AVDexedTheme::oceanTop.withAlpha(alpha), 0.0f, dot.getBottom(), false));
            g.fillEllipse(dot);
        }
        if (AVDexedTheme::showsFocus(button)) {
            g.setColour(AVDexedTheme::oceanHighlight);
            g.drawEllipse(ring.expanded(3.0f), 1.2f);
        }
    }
};

/** GOZO's On/Off choice: two pills, the current one in ocean letters. It
    mirrors (and drives) an existing toggle, which stays hidden, so parameter
    attachments and listeners keep working unchanged. Code that sets the
    toggle silently (dontSendNotification) is picked up by a light poll. */
class OnOffPills final : public juce::Component, private juce::Button::Listener, private juce::Timer {
public:
    explicit OnOffPills(juce::Button& target) : AVTarget(target) {
        for (auto* pill : { &AVOn, &AVOff }) {
            pill->setWantsKeyboardFocus(true);
            addAndMakeVisible(pill);
        }
        AVOn.onClick = [this] { AVChoose(true); };
        AVOff.onClick = [this] { AVChoose(false); };
        AVTarget.addListener(this);
        AVSync();
        startTimerHz(8);
    }
    ~OnOffPills() override { stopTimer(); AVTarget.removeListener(this); }
    void setTexts(const juce::String& on, const juce::String& off) {
        AVOn.setButtonText(on);
        AVOff.setButtonText(off);
        AVOn.setTitle(AVTarget.getTitle() + ": " + on);
        AVOff.setTitle(AVTarget.getTitle() + ": " + off);
        resized();
    }
    /** What the footer shows over either pill (it reads a tooltip before a title). */
    void setHelp(const juce::String& help) {
        AVOn.setTooltip(help);
        AVOff.setTooltip(help);
    }
    /** Width both pills take at the body size, with an 8 px gap. */
    int idealWidth() const {
        return 2 * juce::jmax(pillWidth(AVOn.getButtonText(), 72), pillWidth(AVOff.getButtonText(), 72)) + 8;
    }
    void resized() override {
        const int each = (getWidth() - 8) / 2;
        AVOn.setBounds(0, 0, each, getHeight());
        AVOff.setBounds(each + 8, 0, getWidth() - each - 8, getHeight());
    }
    void enablementChanged() override { AVOn.setEnabled(isEnabled()); AVOff.setEnabled(isEnabled()); }

private:
    juce::Button& AVTarget;
    juce::TextButton AVOn, AVOff;
    void AVChoose(bool on) {
        if (AVTarget.getToggleState() != on) AVTarget.setToggleState(on, juce::sendNotificationSync);
        AVSync();
    }
    void AVSync() {
        AVOn.setToggleState(AVTarget.getToggleState(), juce::dontSendNotification);
        AVOff.setToggleState(!AVTarget.getToggleState(), juce::dontSendNotification);
    }
    void timerCallback() override {
        if (isShowing() && AVOn.getToggleState() != AVTarget.getToggleState()) AVSync();
    }
    void buttonClicked(juce::Button*) override { AVSync(); }
    void buttonStateChanged(juce::Button*) override { AVSync(); }
};
}
