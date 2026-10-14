/**
 *
 * Copyright (c) 2014-2018 Pascal Gauthier.
 *
 * DX7 Envelope Tables from legasynth-0.4.1 / DX7 Patch Editor
 * Copyright (C) 2002 Juan Linietsky <coding@reduz.com.ar>
 * Copyright (C) 2006 Mark-André Hopf <mhopf@mark13.org>
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
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 */

#include "../../debugger/Logger.h"
#include "DXComponents.h"
#include "../DXLookNFeel.h"
#include "../AVDexedTheme.h"
#include "../AVDexedLanguage.h"
#include "PluginProcessor.h"
#include "EnvDisplay.h"

VectorChoiceBox::VectorChoiceBox(Preview preview) : previewType(preview) {
    setWantsKeyboardFocus(true);
    setRepaintsOnMouseActivity(true);
    AVDexedLanguageService::getInstance().addChangeListener(this);
    AVRefreshLanguage();
}

VectorChoiceBox::~VectorChoiceBox() {
    AVDexedLanguageService::getInstance().removeChangeListener(this);
}

void VectorChoiceBox::changeListenerCallback(juce::ChangeBroadcaster*) { AVRefreshLanguage(); }

void VectorChoiceBox::AVRefreshLanguage() {
    auto& language = AVDexedLanguageService::getInstance();
    const int selectedId = getSelectedId();
    clear(dontSendNotification);
    const StringArray keys = previewType == Preview::waveform
        ? StringArray { "triangle", "sawDown", "sawUp", "square", "sine", "sampleHold" }
        : StringArray { "negativeLinear", "negativeExponential", "positiveExponential", "positiveLinear" };
    for (int index = 0; index < keys.size(); ++index)
        addItem(language.text(keys[index]), index + 1);
    setSelectedId(selectedId, dontSendNotification);
    setTitle(language.text(previewType == Preview::waveform ? "lfoWaveform"
                          : previewType == Preview::leftScale ? "leftScaleCurve" : "rightScaleCurve"));
    setTooltip(getTitle() + ": " + getText());
    repaint();
}

void VectorChoiceBox::resized() {
    ComboBox::resized();
    // The parent paints the curve/wave preview. Keep JUCE's label available for
    // its value interface without painting it on top of the diagram.
    for (auto* child : getChildren())
        if (auto* label = dynamic_cast<Label*>(child))
            label->setVisible(false);
}

void VectorChoiceBox::paint(Graphics& g) {
    auto bounds = getLocalBounds().toFloat().reduced(1.0f);
    g.setGradientFill(ColourGradient(AVDexedTheme::controlTop, bounds.getX(), bounds.getY(),
                                    AVDexedTheme::controlBottom, bounds.getX(), bounds.getBottom(), false));
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(AVDexedTheme::showsFocus(*this) ? AVDexedTheme::oceanHighlight : AVDexedTheme::outline);
    g.drawRoundedRectangle(bounds, 4.0f, AVDexedTheme::showsFocus(*this) ? 1.5f : 1.0f);

    const float left = bounds.getX() + 5.0f;
    const bool showName = getWidth() >= 120;
    const float right = showName ? bounds.getX() + 39.0f : bounds.getRight() - 20.0f;
    const float top = bounds.getY() + 5.0f;
    const float bottom = bounds.getBottom() - 5.0f;
    Path preview;
    if (previewType == Preview::waveform) {
        switch (jmax(0, getSelectedItemIndex())) {
            case 0: preview.startNewSubPath(left, bottom); preview.lineTo((left + right) * .5f, top); preview.lineTo(right, bottom); break;
            case 1: preview.startNewSubPath(left, top); preview.lineTo(right, bottom); break;
            case 2: preview.startNewSubPath(left, bottom); preview.lineTo(right, top); break;
            case 3: preview.startNewSubPath(left, top); preview.lineTo((left + right) * .5f, top); preview.lineTo((left + right) * .5f, bottom); preview.lineTo(right, bottom); break;
            case 4: preview.startNewSubPath(left, (top + bottom) * .5f); preview.cubicTo(left + 4, top, right - 4, top, right, (top + bottom) * .5f); break;
            default: preview.startNewSubPath(left, bottom); preview.lineTo(left + 4, top); preview.lineTo(left + 7, bottom); preview.lineTo(left + 11, top); preview.lineTo(left + 14, bottom); preview.lineTo(right, top); break;
        }
    } else {
        const auto index = jmax(0, getSelectedItemIndex());
        const bool exponential = (previewType == Preview::leftScale) ? (index == 1 || index == 2) : (index == 1 || index == 2);
        const bool positive = previewType == Preview::leftScale ? index >= 2 : index >= 2;
        const float mid = (top + bottom) * 0.5f;
        preview.startNewSubPath(left, positive ? bottom : top);
        if (exponential)
            preview.cubicTo(left + 5, positive ? bottom : top, right - 4, positive ? top : bottom, right, positive ? top : bottom);
        else
            preview.lineTo(right, positive ? top : bottom);
        g.setColour(AVDexedTheme::outline.brighter(0.15f));
        g.drawLine(left, mid, right, mid, 0.6f);
    }
    g.setColour(AVDexedTheme::oceanHighlight.withAlpha(isEnabled() ? 1.0f : 0.42f));
    g.strokePath(preview, PathStrokeType(1.5f, PathStrokeType::curved, PathStrokeType::rounded));

    if (showName) {
        g.setFont(AVDexedTheme::font(10.5f));
        g.setColour(AVDexedTheme::textPrimary);
        g.drawText(getText(), Rectangle<float>(right + 8.0f, bounds.getY(), bounds.getRight() - right - 30.0f, bounds.getHeight()), Justification::centredLeft);
    }

    Path arrow;
    arrow.addTriangle(bounds.getRight() - 14.0f, bounds.getCentreY() - 1.0f,
                      bounds.getRight() - 6.0f, bounds.getCentreY() - 1.0f,
                      bounds.getRight() - 10.0f, bounds.getCentreY() + 2.0f);
    g.setColour(AVDexedTheme::textSecondary);
    g.fillPath(arrow);
}

void ProgramSelector::mouseDown(const MouseEvent &event) {
    if (event.mods.isPopupMenu() || event.x < getWidth() - 24 || getNumItems() == 0) {
        ComboBox::mouseDown(event);
        return;
    }
    
    int cur = getSelectedItemIndex();
    if ( event.y < getHeight() / 2 ) {
        if ( cur == 0 )
            cur = getNumItems() - 1;
        else
            cur--;
    } else {
        if ( cur == getNumItems() - 1 )
            cur = 0;
        else
            cur++;
    }
    setSelectedItemIndex(cur);
}

void ProgramSelector::mouseWheelMove(const MouseEvent &event, const MouseWheelDetails &wheel) {
    if (!isEnabled() || getNumItems() == 0) return;
    accum_wheel += wheel.deltaY;

    float wheelFactor = 0.2; // higher is slower
#if JUCE_WINDOWS
    wheelFactor = 0.4;
#endif    
    
    bool up = (accum_wheel > wheelFactor);
    bool dn = (accum_wheel < -wheelFactor);
    int sn = 1;
#if ! JUCE_MAC
    auto t = up;
    up = dn;
    dn = t;
    sn = -1;
#endif    
    
    if( up ) {
        accum_wheel -= sn * wheelFactor;
        int c = getSelectedItemIndex();
        if( c == getNumItems() - 1 ) c = 0;
        else c++;
        setSelectedItemIndex(c);
    }
    else if( dn ) {
        accum_wheel += sn * wheelFactor;
        int c = getSelectedItemIndex();
        if( c == 0 ) c = getNumItems() - 1;
        else c--;
        setSelectedItemIndex(c);
    }
}

void ProgramSelector::paint(Graphics& g) {
    ComboBox::paint(g);
    const auto area = getLocalBounds().toFloat().withTrimmedLeft(static_cast<float>(getWidth() - 24));
    g.setGradientFill(ColourGradient(AVDexedTheme::controlTop, area.getX(), area.getY(),
                                    AVDexedTheme::controlBottom, area.getX(), area.getBottom(), false));
    g.fillRoundedRectangle(area.reduced(2.0f), 4.0f);
    const auto c = area.getCentre();
    Path arrows;
    arrows.addTriangle(c.x - 4.0f, c.y - 3.0f, c.x + 4.0f, c.y - 3.0f, c.x, c.y - 7.0f);
    arrows.addTriangle(c.x - 4.0f, c.y + 3.0f, c.x + 4.0f, c.y + 3.0f, c.x, c.y + 7.0f);
    g.setColour(AVDexedTheme::oceanHighlight.withAlpha(isEnabled() ? 1.0f : 0.42f));
    g.fillPath(arrows);
}
