// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <BinaryData.h>

namespace AVDexedTheme {
inline const juce::Colour canvas { 0xff000000 };
inline const juce::Colour panelTop { 0xff10141c };
inline const juce::Colour panelBottom { 0xff070809 };
inline const juce::Colour textPrimary { 0xffe0e0e0 };
inline const juce::Colour textSecondary { 0xff9a9a9a };
inline const juce::Colour oceanTop { 0xff248fc7 };
inline const juce::Colour oceanBottom { 0xff0b3558 };
inline const juce::Colour oceanHighlight { 0xff58bfe8 };
inline const juce::Colour dangerTop { 0xffb10004 };
inline const juce::Colour dangerBottom { 0xff780002 };
inline const juce::Colour controlTop { 0xff17212a };
inline const juce::Colour controlBottom { 0xff080b0e };
inline const juce::Colour outline { 0xff35434d };
inline const juce::Colour track { 0xff26313a };
inline const juce::Colour banner { 0xff0b0d0f };
inline const juce::Colour silverTop { 0xff666666 };
inline const juce::Colour silverBottom { 0xff424242 };
inline const juce::Colour knobTop { 0xff121212 };
inline const juce::Colour knobBottom { 0xff242424 };
inline const juce::Colour dotTop { 0xff999999 };
inline const juce::Colour dotBottom { 0xffd2d2d2 };
inline juce::Typeface::Ptr typeface(const juce::String& style = "Medium") {
    // A name-based Font resolves via JUCE's global default LookAndFeel. Direct
    // embedded faces keep our font choice scoped to AV Dexed in every host.
    static const auto extraLight = juce::Typeface::createSystemTypefaceFor(BinaryData::MontserratExtraLight_ttf, BinaryData::MontserratExtraLight_ttfSize);
    static const auto light = juce::Typeface::createSystemTypefaceFor(BinaryData::MontserratLight_ttf, BinaryData::MontserratLight_ttfSize);
    static const auto regular = juce::Typeface::createSystemTypefaceFor(BinaryData::MontserratRegular_ttf, BinaryData::MontserratRegular_ttfSize);
    static const auto medium = juce::Typeface::createSystemTypefaceFor(BinaryData::MontserratMedium_ttf, BinaryData::MontserratMedium_ttfSize);
    static const auto semiBold = juce::Typeface::createSystemTypefaceFor(BinaryData::MontserratSemiBold_ttf, BinaryData::MontserratSemiBold_ttfSize);
    static const auto bold = juce::Typeface::createSystemTypefaceFor(BinaryData::MontserratBold_ttf, BinaryData::MontserratBold_ttfSize);
    if (style.containsIgnoreCase("ExtraLight")) return extraLight;
    if (style.containsIgnoreCase("Light")) return light;
    if (style.containsIgnoreCase("SemiBold")) return semiBold;
    if (style.containsIgnoreCase("Bold")) return bold;
    if (style.containsIgnoreCase("Medium")) return medium;
    return regular;
}
inline juce::Font font(float points, const juce::String& style = "Medium") {
    return juce::Font(juce::FontOptions(typeface(style)).withPointHeight(points)).withExtraKerningFactor(-0.04f);
}
/** Focus rings follow the web's :focus-visible rule: they appear after keyboard
    navigation and stay hidden after a mouse click, so clicking a knob or a tab
    leaves no outline behind. The editor turns them on with Tab and off on mouse down. */
inline bool& keyboardFocusVisible() {
    static bool visible = false;
    return visible;
}
inline bool showsFocus(const juce::Component& component) {
    return keyboardFocusVisible() && component.hasKeyboardFocus(true);
}
/** Turns focus rings on; true when that changed. For controls whose own keys
    move the focus (the bank manager's grid of 32 programs). */
inline bool revealKeyboardFocus() {
    if (keyboardFocusVisible()) return false;
    keyboardFocusVisible() = true;
    return true;
}
/** Called by every place that sees keys first (lists, dialogs, the editor).
    Only Tab (and Shift+Tab) counts as keyboard navigation: it is the key that
    moves the focus. Arrow keys choose sounds, nudge a value or move a list's
    selection; they never move the focus, so they must not show a ring on
    whatever control happens to hold it (the Play tab after a click on the top
    bar). Returns true when the rings were turned on. */
inline bool noteKeyboardNavigation(const juce::KeyPress& key) {
    return key.isKeyCode(juce::KeyPress::tabKey) && revealKeyboardFocus();
}
/** Key listener for components that consume navigation keys themselves. */
struct FocusKeyListener final : public juce::KeyListener {
    bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override {
        if (noteKeyboardNavigation(key) && origin != nullptr)
            if (auto* top = origin->getTopLevelComponent()) top->repaint();
        return false;
    }
};
/** Vertical text fill from `top` to `bottom` over y1..y2. CoreGraphics paints a
    gradient glyph run as one clipped outline per glyph, and on 1x displays thin
    stems such as "l" or "-" can drop out of small text. Below ~16 device pixels
    the text uses one solid tone between both stops, which CoreText rasterises
    normally; larger text keeps the gradient. */
inline void textFill(juce::Graphics& g, const juce::Font& textFont, juce::Colour top, juce::Colour bottom,
                     float y1, float y2) {
    if (textFont.getHeight() * g.getInternalContext().getPhysicalPixelScaleFactor() < 16.0f) {
        g.setColour(top.interpolatedWith(bottom, 0.4f));
        return;
    }
    g.setGradientFill(juce::ColourGradient(top, 0.0f, y1, bottom, 0.0f, y2, false));
}
/** Figma-style corner radius: never more than half the shorter side. */
inline float cornerRadius(juce::Rectangle<float> area, float radius) {
    return juce::jmin(radius, juce::jmin(area.getWidth(), area.getHeight()) * 0.5f);
}
/** The brand's neutral box: the grey text gradient at 12%, no outline. Hover
    "lifts" it (brighter and a little more opaque) instead of tinting it. */
inline void fillBox(juce::Graphics& g, juce::Rectangle<float> area, float radius,
                    bool lit = false, float alpha = 1.0f) {
    if (area.isEmpty()) return;
    const auto top = lit ? textPrimary.brighter(0.18f) : textPrimary;
    const auto bottom = lit ? textSecondary.brighter(0.18f) : textSecondary;
    juce::ColourGradient gradient(top, 0.0f, area.getY(), bottom, 0.0f, area.getBottom(), false);
    gradient.multiplyOpacity(0.12f * (lit ? 1.18f : 1.0f) * alpha);
    g.setGradientFill(gradient);
    if (radius <= 0.0f) g.fillRect(area);
    else g.fillRoundedRectangle(area, cornerRadius(area, radius));
}
/** Gradient text on one line. Text that doesn't fit shrinks (down to 55%)
    rather than being squashed, which would make thin glyphs vanish. */
inline void drawText(juce::Graphics& g, const juce::String& text, juce::Font textFont,
                     juce::Rectangle<float> area, juce::Colour top, juce::Colour bottom,
                     juce::Justification justification = juce::Justification::centred, float alpha = 1.0f) {
    const auto width = juce::GlyphArrangement::getStringWidth(textFont, text);
    if (width > area.getWidth() && width > 0.0f)
        textFont = textFont.withHeight(textFont.getHeight() * juce::jmax(0.55f, area.getWidth() / width));
    const auto band = area.withSizeKeepingCentre(area.getWidth(), juce::jmin(area.getHeight(), textFont.getHeight()));
    g.setFont(textFont);
    textFill(g, textFont, top.withMultipliedAlpha(alpha), bottom.withMultipliedAlpha(alpha), band.getY(), band.getBottom());
    g.drawText(text, area, justification, true);
}
inline void drawGreyText(juce::Graphics& g, const juce::String& text, const juce::Font& textFont,
                         juce::Rectangle<float> area,
                         juce::Justification justification = juce::Justification::centred, float alpha = 1.0f) {
    drawText(g, text, textFont, area, textPrimary, textSecondary, justification, alpha);
}
inline void drawOceanText(juce::Graphics& g, const juce::String& text, const juce::Font& textFont,
                          juce::Rectangle<float> area,
                          juce::Justification justification = juce::Justification::centred, float alpha = 1.0f) {
    drawText(g, text, textFont, area, oceanHighlight, oceanTop, justification, alpha);
}
/** Selected list row (full) and hovered row (×0.21): accent from 15% on the
    left to 58% on the right, radius 6. */
inline void fillRowHighlight(juce::Graphics& g, juce::Rectangle<float> row, float strength = 1.0f) {
    juce::ColourGradient gradient(oceanHighlight.withAlpha(0.15f), row.getX(), row.getCentreY(),
                                  oceanHighlight.withAlpha(0.58f), row.getRight(), row.getCentreY(), false);
    gradient.multiplyOpacity(strength);
    g.setGradientFill(gradient);
    g.fillRoundedRectangle(row, cornerRadius(row, 6.0f));
}
/** Blurred backdrop for in-editor modals (Settings, effects, prompts): a 1/3
    copy of a snapshot with a 9-tap gaussian. Drawn stretched, under a 54% veil. */
inline juce::Image blurredBackdrop(const juce::Image& snapshot) {
    if (! snapshot.isValid()) return {};
    const auto small = snapshot.rescaled(juce::jmax(1, snapshot.getWidth() / 3), juce::jmax(1, snapshot.getHeight() / 3));
    auto blurred = small.createCopy();
    juce::ImageConvolutionKernel kernel(9);
    kernel.createGaussianBlur(2.0f);
    kernel.applyToImage(blurred, small, small.getBounds());
    return blurred;
}
/** Card/side-panel/modal surface: the vertical panel gradient, no outline. */
inline void panel(juce::Graphics& g, juce::Rectangle<float> area, float radius = 9.0f) {
    g.setGradientFill(juce::ColourGradient(panelTop, area.getX(), area.getY(),
                                          panelBottom, area.getX(), area.getBottom(), false));
    if (radius <= 0.0f) g.fillRect(area);
    else g.fillRoundedRectangle(area, cornerRadius(area, radius));
}
}
