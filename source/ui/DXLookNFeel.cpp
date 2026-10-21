/**
 *
 * Copyright (c) 2013-2024 Pascal Gauthier.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
 *
 * Modified by AV Vibes, 2026 (AV Dexed): AV Vibes look and feel.
 */

#include "DXLookNFeel.h"
#include "AVDexedTheme.h"
#include "AVDexedKnob.h"
#include "AVDexedLanguage.h"
#include "../JuceLibraryCode/BinaryData.h"
#include "../debugger/Logger.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"

#define REG_COLOUR(id, value) setColour(id, value)

DXLookNFeel::DXLookNFeel() {

    REG_COLOUR(TextButton::buttonColourId, AVDexedTheme::oceanBottom);
    REG_COLOUR(TextButton::buttonOnColourId, AVDexedTheme::oceanTop);
    REG_COLOUR(TextButton::textColourOnId, AVDexedTheme::textPrimary);
    REG_COLOUR(TextButton::textColourOffId, AVDexedTheme::textPrimary);
    REG_COLOUR(ToggleButton::textColourId, AVDexedTheme::textPrimary);
    REG_COLOUR(ToggleButton::tickColourId, AVDexedTheme::oceanHighlight);
    REG_COLOUR(ToggleButton::tickDisabledColourId, AVDexedTheme::textSecondary.withAlpha(0.42f));
    REG_COLOUR(Slider::rotarySliderOutlineColourId, AVDexedTheme::outline);
    REG_COLOUR(Slider::rotarySliderFillColourId, AVDexedTheme::oceanTop);
    REG_COLOUR(AlertWindow::backgroundColourId, AVDexedTheme::panelTop);
    REG_COLOUR(AlertWindow::textColourId, AVDexedTheme::textPrimary);
    REG_COLOUR(TextEditor::backgroundColourId, AVDexedTheme::controlBottom);
    REG_COLOUR(TextEditor::textColourId, AVDexedTheme::textPrimary);
    REG_COLOUR(TextEditor::highlightColourId, AVDexedTheme::oceanBottom);
    REG_COLOUR(TextEditor::outlineColourId, AVDexedTheme::outline);
    REG_COLOUR(TextEditor::focusedOutlineColourId, AVDexedTheme::oceanHighlight);
    REG_COLOUR(ComboBox::backgroundColourId, AVDexedTheme::controlBottom);
    REG_COLOUR(ComboBox::textColourId, AVDexedTheme::textPrimary);
    REG_COLOUR(ComboBox::buttonColourId, AVDexedTheme::oceanHighlight);
    REG_COLOUR(PopupMenu::backgroundColourId, Colours::transparentBlack);
    REG_COLOUR(PopupMenu::textColourId, AVDexedTheme::textPrimary);
    REG_COLOUR(PopupMenu::highlightedTextColourId, AVDexedTheme::textPrimary);
    REG_COLOUR(PopupMenu::highlightedBackgroundColourId, AVDexedTheme::oceanBottom);
    REG_COLOUR(TreeView::backgroundColourId, Colours::transparentBlack);
    REG_COLOUR(DirectoryContentsDisplayComponent::highlightColourId, AVDexedTheme::oceanTop.withAlpha(0.45f));
    REG_COLOUR(DirectoryContentsDisplayComponent::textColourId, AVDexedTheme::textPrimary);
    REG_COLOUR(DialogWindow::backgroundColourId, AVDexedTheme::panelTop);
    REG_COLOUR(ListBox::backgroundColourId, AVDexedTheme::controlBottom);
    REG_COLOUR(ListBox::textColourId, AVDexedTheme::textPrimary);
    REG_COLOUR(Label::textColourId, AVDexedTheme::textPrimary);
    REG_COLOUR(TableHeaderComponent::backgroundColourId, AVDexedTheme::controlTop);
    REG_COLOUR(TableHeaderComponent::textColourId, AVDexedTheme::textPrimary);
    REG_COLOUR(TableHeaderComponent::outlineColourId, AVDexedTheme::outline);
    REG_COLOUR(TableHeaderComponent::highlightColourId, AVDexedTheme::oceanBottom);
    REG_COLOUR(ScrollBar::thumbColourId, AVDexedTheme::oceanBottom);

}

Typeface::Ptr DXLookNFeel::getTypefaceForFont(const Font& font) {
    if (font.getTypefaceName() == Font::getDefaultSansSerifFontName() || font.getTypefaceName() == "Montserrat") {
        return AVDexedTheme::typeface(font.getTypefaceStyle());
    }
    return LookAndFeel_V4::getTypefaceForFont(font);
}

Font DXLookNFeel::getTextButtonFont(TextButton&, int height) { return AVDexedTheme::font(jlimit(11.5f, 15.0f, float(height) * 0.46f)); }
Font DXLookNFeel::getComboBoxFont(ComboBox& box) { return AVDexedTheme::font(jlimit(11.5f, 15.0f, float(box.getHeight()) * 0.48f)); }
Font DXLookNFeel::getPopupMenuFont() { return AVDexedTheme::font(15.0f); }
Font DXLookNFeel::getAlertWindowTitleFont() { return AVDexedTheme::font(20.0f, "SemiBold"); }
Font DXLookNFeel::getAlertWindowMessageFont() { return AVDexedTheme::font(15.0f); }
Font DXLookNFeel::getAlertWindowFont() { return AVDexedTheme::font(15.0f); }

void DXLookNFeel::drawButtonText(Graphics& g, TextButton& button, bool hover, bool down) {
    // Hover, press and the "on" state paint the letters ocean; the box never
    // turns blue.
    const bool tab = bool(button.getProperties()["AVTab"]);
    const auto font = tab ? AVDexedTheme::font(float(double(button.getProperties().getWithDefault("AVTabSize", 30.0))), "SemiBold")
                          : getTextButtonFont(button, button.getHeight());
    const bool lit = button.getToggleState() || (button.isEnabled() && (hover || down));
    const float alpha = button.isEnabled() ? 1.0f : 0.42f;
    const auto area = button.getLocalBounds().toFloat().reduced(tab ? 0.0f : 8.0f, 1.0f);
    if (lit) AVDexedTheme::drawOceanText(g, button.getButtonText(), font, area, Justification::centred, alpha);
    else AVDexedTheme::drawGreyText(g, button.getButtonText(), font, area, Justification::centred, alpha);
}

void DXLookNFeel::drawDrawableButton(Graphics& g, DrawableButton& button, bool, bool) {
    // JUCE attaches the selected Drawable as a child and paints it after this
    // background callback. Painting it here as well produces doubled icons.
    if (AVDexedTheme::showsFocus(button)) {
        g.setColour(AVDexedTheme::oceanHighlight);
        g.drawRoundedRectangle(button.getLocalBounds().toFloat().reduced(1), 4, 1.0f);
    }
}

Label* DXLookNFeel::createSliderTextBox(Slider& slider) {
    auto* label = LookAndFeel_V4::createSliderTextBox(slider);
    label->setFont(AVDexedTheme::font(11.0f));
    label->setColour(Label::textColourId, AVDexedTheme::textPrimary);
    // Default value box: the neutral 10–12% fill, no outline (GOZO value boxes).
    label->setColour(Label::backgroundColourId, slider.isColourSpecified(Slider::textBoxBackgroundColourId) ? slider.findColour(Slider::textBoxBackgroundColourId) : AVDexedTheme::textPrimary.withAlpha(0.10f));
    label->setColour(Label::outlineColourId, slider.isColourSpecified(Slider::textBoxOutlineColourId) ? slider.findColour(Slider::textBoxOutlineColourId) : Colours::transparentBlack);
    label->setColour(TextEditor::focusedOutlineColourId, AVDexedTheme::oceanHighlight);
    label->setJustificationType(Justification::centred);
    label->setBorderSize(BorderSize<int>(1, 3, 1, 3));
    return label;
}

void DXLookNFeel::drawLabel(Graphics& g, Label& label) {
    const auto bounds = label.getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(label.findColour(Label::backgroundColourId));
    g.fillRoundedRectangle(bounds, 4.0f);
    const auto outline = AVDexedTheme::showsFocus(label) ? AVDexedTheme::oceanHighlight
                                                     : label.findColour(Label::outlineColourId);
    g.setColour(outline);
    g.drawRoundedRectangle(bounds, 4.0f, AVDexedTheme::showsFocus(label) ? 1.3f : 0.8f);
    if (!label.isBeingEdited()) {
        const auto font = AVDexedTheme::font(jmax(10.5f, label.getFont().getHeightInPoints()), label.getFont().getTypefaceStyle());
        g.setFont(font);
        const auto textArea = label.getBorderSize().subtractedFrom(label.getLocalBounds());
        const auto colour = label.findColour(Label::textColourId);
        const float alpha = label.isEnabled() ? 1.0f : 0.42f;
        bool gradient = false;
        if (colour == AVDexedTheme::textPrimary || colour == AVDexedTheme::textSecondary) {
            const float top = textArea.getCentreY() - font.getHeight() * 0.5f;
            gradient = AVDexedTheme::textFill(g, font, AVDexedTheme::textPrimary.withAlpha(alpha),
                                              AVDexedTheme::textSecondary.withAlpha(alpha), top, top + font.getHeight());
        } else g.setColour(colour.withMultipliedAlpha(alpha));
        AVDexedTheme::drawTextLine(g, label.getText(), textArea.toFloat(), label.getJustificationType(), gradient);
    }
}

void AVShowMessage(MessageBoxIconType icon, const String& title, const String& message, Component* parent) {
    // Dialogs live on the message thread. A caller on another thread (a host
    // restoring state) must not touch `parent` or the editor's dialog queue
    // there: the notice follows on the message thread, as an alert window.
    if (! MessageManager::existsAndIsCurrentThread()) {
        MessageManager::callAsync([icon, title, message] { AVShowMessage(icon, title, message, nullptr); });
        return;
    }
    // Inside an editor the notice is one of its own dialogs (blurred backdrop,
    // follows the zoom, cannot be dragged away, waits for an open prompt).
    auto* editor = dynamic_cast<DexedAudioProcessorEditor*>(parent);
    if (editor == nullptr && parent != nullptr)
        editor = parent->findParentComponentOfClass<DexedAudioProcessorEditor>();
    if (editor != nullptr) {
        editor->showAVNotice(title, message);
        return;
    }
    // No editor (a desktop window such as the tuning table, or none open):
    // the AV-themed alert window.
    auto* alert = new AVAlertWindow(title, message, icon, parent);
    alert->addButton(AVDexedLanguageService::getInstance().text("ok"), 0,
                     KeyPress(KeyPress::returnKey), KeyPress(KeyPress::escapeKey));
    alert->enterModalState(true, nullptr, true);
}

void DXLookNFeel::drawAlertBox(Graphics& g, AlertWindow& alert,
                              const Rectangle<int>& textArea, TextLayout& layout) {
    AVDexedTheme::panel(g, alert.getLocalBounds().toFloat(), 10.0f);
    layout.draw(g, textArea.toFloat());
    if (AVDexedTheme::showsFocus(alert)) {
        g.setColour(AVDexedTheme::oceanHighlight.withAlpha(0.7f));
        g.drawRoundedRectangle(alert.getLocalBounds().toFloat().reduced(1.0f), 9.0f, 1.0f);
    }
}

void DXLookNFeel::drawTooltip(Graphics& g, const String& text, int width, int height) {
    AVDexedTheme::panel(g, Rectangle<float>(static_cast<float>(width), static_cast<float>(height)), 6.0f);
    AttributedString content;
    content.setJustification(Justification::centred);
    content.append(text, AVDexedTheme::font(11.0f, "Regular"), AVDexedTheme::textPrimary);
    TextLayout layout;
    layout.createLayout(content, static_cast<float>(width - 16));
    layout.draw(g, Rectangle<float>(8.0f, 4.0f, static_cast<float>(width - 16), static_cast<float>(height - 8)));
}

Rectangle<int> DXLookNFeel::getTooltipBounds(const String& text, Point<int> position, Rectangle<int> parent) {
    AttributedString content;
    content.append(text, AVDexedTheme::font(11.0f, "Regular"), AVDexedTheme::textPrimary);
    TextLayout layout;
    layout.createLayout(content, static_cast<float>(jmin(440, parent.getWidth() - 24)));
    const int width = roundToInt(layout.getWidth()) + 20;
    const int height = roundToInt(layout.getHeight()) + 14;
    return Rectangle<int>(position.x + 14, position.y + 22, width, height).constrainedWithin(parent);
}

Font DXLookNFeel::getTabButtonFont(TabBarButton&, float) { return AVDexedTheme::font(11.5f); }

void DXLookNFeel::drawTableHeaderBackground(Graphics& g, TableHeaderComponent& header) {
    g.setGradientFill(ColourGradient(AVDexedTheme::controlTop, 0.0f, 0.0f, AVDexedTheme::controlBottom,
                                    0.0f, static_cast<float>(header.getHeight()), false));
    g.fillAll();
}

void DXLookNFeel::drawTableHeaderColumn(Graphics& g, TableHeaderComponent&, const String& name, int,
                                       int width, int height, bool hover, bool down, int) {
    if (hover || down) {
        g.setColour(AVDexedTheme::oceanBottom.withAlpha(down ? 0.8f : 0.45f));
        g.fillRect(0, 0, width, height);
    }
    g.setColour(AVDexedTheme::textPrimary);
    g.setFont(AVDexedTheme::font(11.0f, "SemiBold"));
    g.drawText(name, 10, 0, width - 20, height, Justification::centredLeft);
    g.setColour(AVDexedTheme::outline);
    g.drawVerticalLine(width - 1, 0, static_cast<float>(height));
}

void DXLookNFeel::drawRotarySlider(Graphics& g, int x, int y, int width, int height,
                                             float position, float startAngle, float endAngle, Slider& slider) {
    ignoreUnused(startAngle, endAngle);
    AVDrawKnob(g, Rectangle<float>(float(x), float(y), float(width), float(height)).reduced(2.0f), position, slider);
}

void DXLookNFeel::drawToggleButton(Graphics& g, ToggleButton& button,
                                   bool isMouseOverButton, bool isButtonDown) {
    auto bounds = button.getLocalBounds().toFloat().reduced(2.0f);
    const bool isOn = button.getToggleState();
    const auto body = bounds.reduced(1.0f);
    const float alpha = button.isEnabled() ? 1.0f : 0.42f;
    if (isOn) {
        auto top = AVDexedTheme::oceanTop, bottom = AVDexedTheme::oceanBottom;
        if (isMouseOverButton) { top = top.brighter(0.12f); bottom = bottom.brighter(0.12f); }
        g.setGradientFill(ColourGradient(top.withMultipliedAlpha(alpha), 0.0f, body.getY(),
                                         bottom.withMultipliedAlpha(alpha), 0.0f, body.getBottom(), false));
        g.fillRoundedRectangle(body, body.getHeight() * 0.5f);
    } else {
        AVDexedTheme::fillBox(g, body, body.getHeight() * 0.5f, isMouseOverButton && button.isEnabled(), alpha * 1.6f);
    }
    if (AVDexedTheme::showsFocus(button)) {
        g.setColour(AVDexedTheme::oceanHighlight);
        g.drawRoundedRectangle(body, body.getHeight() * 0.5f, 1.4f);
    }
    const float thumbDiameter = jmax(4.0f, body.getHeight() - 6.0f);
    const float thumbX = isOn ? body.getRight() - thumbDiameter - 3.0f : body.getX() + 3.0f;
    auto thumb = Rectangle<float>(thumbDiameter, thumbDiameter).withPosition(thumbX, body.getCentreY() - thumbDiameter * 0.5f);
    g.setColour(Colours::black.withAlpha(0.45f * alpha));
    g.fillEllipse(thumb.translated(0.0f, isButtonDown ? 0.5f : 1.0f));
    g.setGradientFill(ColourGradient(AVDexedTheme::textPrimary.withMultipliedAlpha(alpha), thumb.getX(), thumb.getY(),
                                     AVDexedTheme::textSecondary.withMultipliedAlpha(alpha), thumb.getX(), thumb.getBottom(), false));
    g.fillEllipse(thumb);
}

void DXLookNFeel::drawButtonBackground(Graphics& g, Button& button,
                                       const Colour&, bool isMouseOverButton, bool isButtonDown) {
    const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    // Tabs and text links are bare letters; everything else is the 12% box
    // that lifts on hover.
    if (!bool(button.getProperties()["AVTab"]) && !bool(button.getProperties()["AVLink"])) {
        const bool lit = button.isEnabled() && isMouseOverButton && !isButtonDown;
        AVDexedTheme::fillBox(g, bounds, 9.0f, lit, button.isEnabled() ? 1.0f : 0.5f);
    }
    if (AVDexedTheme::showsFocus(button)) {
        g.setColour(AVDexedTheme::oceanHighlight);
        g.drawRoundedRectangle(bounds, AVDexedTheme::cornerRadius(bounds, 9.0f), 1.2f);
    }
}

void DXLookNFeel::drawLinearSlider(Graphics& g, int x, int y, int width, int height,
                                   float sliderPos, float minSliderPos, float maxSliderPos,
                                   const Slider::SliderStyle style, Slider& slider) {
    if (style != Slider::LinearHorizontal) {
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos,
                                         minSliderPos, maxSliderPos, style, slider);
        return;
    }
    // Settings fader: 6 px track in the neutral box, ocean fill, ocean thumb.
    // The track covers exactly the span sliderPos runs over. Drawn 6 px in from
    // each end, it left the thumb 6 px past its ends (and in Settings, where
    // the general volume rests at its maximum, cut in half by the bounds).
    const auto area = Rectangle<float>(float(x), float(y), float(width), float(height));
    const auto track = AVFaderTrack(area);
    AVDexedTheme::fillBox(g, track, 3.0f, slider.isMouseOverOrDragging(), 1.4f);
    const auto active = track.withWidth(jlimit(0.0f, track.getWidth(), sliderPos - track.getX()));
    g.setGradientFill(ColourGradient(AVDexedTheme::oceanTop, 0.0f, track.getY(),
                                     AVDexedTheme::oceanBottom, 0.0f, track.getBottom(), false));
    g.fillRoundedRectangle(active, 3.0f);
    const auto thumb = Rectangle<float>(AVFaderThumbRadius * 2.0f, AVFaderThumbRadius * 2.0f)
                           .withCentre({ sliderPos, track.getCentreY() });
    g.setGradientFill(ColourGradient(AVDexedTheme::oceanHighlight, 0.0f, thumb.getY(),
                                     AVDexedTheme::oceanTop, 0.0f, thumb.getBottom(), false));
    g.fillEllipse(thumb);
    if (AVDexedTheme::showsFocus(slider)) {
        // Around the thumb's whole travel, kept inside the slider.
        const auto ring = area.expanded(float(AVFaderInset), 0.0f)
                              .getIntersection(slider.getLocalBounds().toFloat().reduced(0.6f));
        g.setColour(AVDexedTheme::oceanHighlight.withAlpha(0.65f));
        g.drawRoundedRectangle(ring, 4.0f, 1.2f);
    }
}

Rectangle<float> DXLookNFeel::AVFaderTrack(Rectangle<float> sliderArea) {
    return { sliderArea.getX(), sliderArea.getCentreY() - 3.0f, jmax(1.0f, sliderArea.getWidth()), 6.0f };
}

void DXLookNFeel::positionComboBoxText(ComboBox& box, Label& label) {
    label.setBounds(10, 1, jmax(1, box.getWidth() - 38), jmax(1, box.getHeight() - 2));
    label.setFont(getComboBoxFont(box));
    label.setJustificationType(box.getJustificationType());
    label.setBorderSize(BorderSize<int>());
}

Label* DXLookNFeel::createComboBoxTextBox(ComboBox& box) {
    // The box lifts on hover, so it has to repaint when the pointer crosses it.
    box.setRepaintsOnMouseActivity(true);
    return LookAndFeel_V4::createComboBoxTextBox(box);
}

void DXLookNFeel::drawComboBox(Graphics& g, int width, int height, bool isButtonDown,
                               int, int, int, int, ComboBox& box) {
    const auto bounds = Rectangle<float>(0.0f, 0.0f, float(width), float(height)).reduced(0.5f);
    const float alpha = box.isEnabled() ? 1.0f : 0.5f;
    AVDexedTheme::fillBox(g, bounds, 6.0f, box.isEnabled() && (box.isMouseOver(true) || isButtonDown), alpha);
    if (AVDexedTheme::showsFocus(box)) {
        g.setColour(AVDexedTheme::oceanHighlight);
        g.drawRoundedRectangle(bounds, AVDexedTheme::cornerRadius(bounds, 6.0f), 1.2f);
    }
    const float cx = float(width) - 14.0f;
    const float cy = float(height) * 0.5f - 1.0f;
    Path chevron;
    chevron.startNewSubPath(cx - 5.0f, cy - 2.5f);
    chevron.lineTo(cx, cy + 2.5f);
    chevron.lineTo(cx + 5.0f, cy - 2.5f);
    g.setColour(AVDexedTheme::oceanHighlight.withAlpha(alpha));
    g.strokePath(chevron, PathStrokeType(2.0f, PathStrokeType::curved, PathStrokeType::rounded));
}

void DXLookNFeel::drawPopupMenuBackgroundWithOptions(Graphics& g, int width, int height,
                                                      const PopupMenu::Options&) {
    const auto area = Rectangle<float>(0.0f, 0.0f, float(width), float(height));
    AVDexedTheme::panel(g, area, Desktop::canUseSemiTransparentWindows() ? 6.0f : 0.0f);
    ColourGradient edge(AVDexedTheme::textPrimary, 0.0f, 0.0f, AVDexedTheme::textSecondary, 0.0f, float(height), false);
    edge.multiplyOpacity(0.25f);
    g.setGradientFill(edge);
    g.drawRoundedRectangle(area.reduced(0.5f), AVDexedTheme::cornerRadius(area, 6.0f), 1.0f);
}

void DXLookNFeel::drawPopupMenuItemWithOptions(Graphics& g, const Rectangle<int>& area,
                                                bool isHighlighted, const PopupMenu::Item& item,
                                                const PopupMenu::Options&) {
    constexpr float padX = 14.0f, tickW = 22.0f;
    const auto r = area.toFloat();
    if (item.isSeparator) {
        const auto line = r.withSizeKeepingCentre(r.getWidth() - padX * 2.0f, 1.0f);
        ColourGradient grey(AVDexedTheme::textPrimary, 0.0f, line.getY(), AVDexedTheme::textSecondary, 0.0f, line.getBottom(), false);
        grey.multiplyOpacity(0.22f);
        g.setGradientFill(grey);
        g.fillRect(line);
        return;
    }
    const float alpha = item.isEnabled ? 1.0f : 0.40f;
    if (isHighlighted && item.isEnabled) {
        const auto highlight = r.reduced(3.0f, 1.0f);
        ColourGradient fill(AVDexedTheme::oceanTop.withAlpha(0.0f), highlight.getX(), highlight.getCentreY(),
                            AVDexedTheme::oceanTop, highlight.getRight(), highlight.getCentreY(), false);
        fill.multiplyOpacity(0.30f);
        g.setGradientFill(fill);
        g.fillRoundedRectangle(highlight, AVDexedTheme::cornerRadius(highlight, 6.0f));
    }
    const auto tickArea = Rectangle<float>(r.getX() + padX - 4.0f, r.getY(), tickW, r.getHeight());
    if (item.image != nullptr) {
        item.image->drawWithin(g, tickArea.withSizeKeepingCentre(14.0f, 14.0f), RectanglePlacement::centred, alpha);
    } else if (item.isTicked) {
        const float cx = r.getX() + padX + 4.0f, cy = r.getCentreY();
        Path tick;
        tick.startNewSubPath(cx - 4.0f, cy);
        tick.lineTo(cx - 1.0f, cy + 3.5f);
        tick.lineTo(cx + 5.0f, cy - 4.0f);
        g.setColour(AVDexedTheme::oceanHighlight.withAlpha(alpha));
        g.strokePath(tick, PathStrokeType(2.0f, PathStrokeType::curved, PathStrokeType::rounded));
    }
    auto textArea = r.withTrimmedLeft(padX + tickW).withTrimmedRight(padX);
    if (item.subMenu != nullptr)
        textArea = textArea.withTrimmedRight(18.0f);
    if (item.shortcutKeyDescription.isNotEmpty()) {
        const auto shortcutFont = AVDexedTheme::font(12.0f);
        AVDexedTheme::drawGreyText(g, item.shortcutKeyDescription, shortcutFont, r.withTrimmedRight(padX),
                                   Justification::centredRight, alpha * 0.6f);
        textArea = textArea.withTrimmedRight(GlyphArrangement::getStringWidth(shortcutFont, item.shortcutKeyDescription) + 12.0f);
    }
    if (item.colour != Colour()) {
        g.setColour(item.colour.withMultipliedAlpha(alpha));
        g.setFont(getPopupMenuFont());
        g.drawText(item.text, textArea, Justification::centredLeft, true);
    } else if (item.isTicked && item.image != nullptr) {
        // Items with an icon have no room for the tick: the chosen one reads in ocean.
        AVDexedTheme::drawOceanText(g, item.text, getPopupMenuFont(), textArea, Justification::centredLeft, alpha);
    } else {
        AVDexedTheme::drawGreyText(g, item.text, getPopupMenuFont(), textArea, Justification::centredLeft, alpha);
    }
    if (item.subMenu != nullptr) {
        const float cx = r.getRight() - padX - 3.0f, cy = r.getCentreY();
        Path chevron;
        chevron.startNewSubPath(cx - 2.5f, cy - 5.0f);
        chevron.lineTo(cx + 2.5f, cy);
        chevron.lineTo(cx - 2.5f, cy + 5.0f);
        g.setColour(AVDexedTheme::oceanHighlight.withAlpha(alpha * (isHighlighted ? 1.0f : 0.75f)));
        g.strokePath(chevron, PathStrokeType(2.0f, PathStrokeType::curved, PathStrokeType::rounded));
    }
}

void DXLookNFeel::getIdealPopupMenuItemSizeWithOptions(const String& text, bool separator,
                                                        int standardHeight, int& idealWidth, int& idealHeight,
                                                        const PopupMenu::Options&) {
    if (separator) {
        idealWidth = 60;
        idealHeight = standardHeight > 0 ? standardHeight / 2 : 9;
        return;
    }
    idealHeight = 30;
    idealWidth = roundToInt(GlyphArrangement::getStringWidth(getPopupMenuFont(), text)) + roundToInt(22.0f + 14.0f * 2.0f + 22.0f);
}

int DXLookNFeel::getPopupMenuBorderSizeWithOptions(const PopupMenu::Options&) {
    return 6;
}

void DXLookNFeel::drawResizableFrame(Graphics&, int, int, const BorderSize<int>&) {
    // The rounded outline is painted by drawPopupMenuBackgroundWithOptions.
}

Colour DXLookNFeel::fillColour = AVDexedTheme::oceanTop;
Colour DXLookNFeel::lightBackground = AVDexedTheme::panelTop;
Colour DXLookNFeel::background = AVDexedTheme::canvas;
Colour DXLookNFeel::roundBackground = AVDexedTheme::panelTop;
