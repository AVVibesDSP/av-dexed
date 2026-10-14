/**
 *
 * Copyright (c) 2013-2018 Pascal Gauthier.
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

#ifndef DXLOOKNFEEL_H_INCLUDED
#define DXLOOKNFEEL_H_INCLUDED

#include "../JuceLibraryCode/JuceHeader.h"
#include "AVDexedTheme.h"

class LightedToggleButton : public ToggleButton {
public:
    LightedToggleButton(juce::String name) : ToggleButton(name) { }
};


class DXLookNFeel : public LookAndFeel_V4 {
public:
    DXLookNFeel();
    
    /* overridden methods */
    virtual void drawRotarySlider(Graphics &g, int x, int y, int width, int height, float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,  Slider &slider ) override;
    virtual void drawToggleButton(Graphics& g, ToggleButton& button, bool isMouseOverButton, bool isButtonDown) override;
    
    virtual void drawButtonBackground (Graphics&, Button&, const Colour& backgroundColour,
                               bool isMouseOverButton, bool isButtonDown) override;
    virtual void drawLinearSlider (Graphics&, int x, int y, int width, int height,
                                 float sliderPos, float minSliderPos, float maxSliderPos,
                                 const Slider::SliderStyle, Slider&) override;

    Font getTextButtonFont(TextButton&, int buttonHeight) override;
    void drawButtonText(Graphics&, TextButton&, bool, bool) override;
    void drawDrawableButton(Graphics&, DrawableButton&, bool, bool) override;
    Font getComboBoxFont(ComboBox&) override;
    Font getPopupMenuFont() override;
    Font getAlertWindowTitleFont() override;
    Font getAlertWindowMessageFont() override;
    Font getAlertWindowFont() override;
    Label* createSliderTextBox(Slider&) override;
    void drawLabel(Graphics&, Label&) override;
    void drawAlertBox(Graphics&, AlertWindow&, const Rectangle<int>&, TextLayout&) override;
    void drawTooltip(Graphics&, const String&, int width, int height) override;
    Rectangle<int> getTooltipBounds(const String&, juce::Point<int>, Rectangle<int>) override;
    Font getTabButtonFont(TabBarButton&, float height) override;
    void drawTableHeaderBackground(Graphics&, TableHeaderComponent&) override;
    void drawTableHeaderColumn(Graphics&, TableHeaderComponent&, const String&, int columnId,
                               int width, int height, bool isMouseOver, bool isMouseDown, int columnFlags) override;
    virtual Typeface::Ptr getTypefaceForFont(const Font &) override;
    virtual void positionComboBoxText (ComboBox& box, Label& label) override;
    Label* createComboBoxTextBox(ComboBox& box) override;
    void drawComboBox(Graphics&, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      ComboBox&) override;
    void drawPopupMenuBackgroundWithOptions(Graphics&, int width, int height,
                                            const PopupMenu::Options&) override;
    void drawPopupMenuItemWithOptions(Graphics&, const Rectangle<int>& area,
                                      bool isHighlighted, const PopupMenu::Item&,
                                      const PopupMenu::Options&) override;
    int getPopupMenuBorderSizeWithOptions(const PopupMenu::Options&) override;
    void getIdealPopupMenuItemSizeWithOptions(const String&, bool isSeparator,
                                               int standardMenuItemHeight,
                                               int& idealWidth, int& idealHeight,
                                               const PopupMenu::Options&) override;
    void drawResizableFrame(Graphics&, int width, int height,
                            const BorderSize<int>& border) override;

    static Colour fillColour;
    static Colour lightBackground;
    static Colour background;
    static Colour roundBackground;
};

/** A modal owns its theme so closing the originating editor cannot leave a
    dangling LookAndFeel pointer in JUCE's asynchronous window. While it is
    open, the editor it belongs to sits under a 55% black veil, as GOZO dims
    the instrument behind its dialogs. */
class AVAlertWindow : public juce::AlertWindow {
public:
    AVAlertWindow(const juce::String& title, const juce::String& message,
                  juce::MessageBoxIconType icon, juce::Component* parent = nullptr)
        : juce::AlertWindow(title, message, icon, parent) {
        setLookAndFeel(&AVLookAndFeel);
        auto* editor = dynamic_cast<juce::AudioProcessorEditor*>(parent);
        if (editor == nullptr && parent != nullptr)
            editor = parent->findParentComponentOfClass<juce::AudioProcessorEditor>();
        if (editor != nullptr) {
            AVVeil = std::make_unique<Veil>(*editor);
            juce::Desktop::getInstance().getAnimator().fadeIn(AVVeil.get(), 83);
        }
    }
    ~AVAlertWindow() override {
        if (AVVeil != nullptr)
            juce::Desktop::getInstance().getAnimator().cancelAnimation(AVVeil.get(), false);
        setLookAndFeel(nullptr);
    }
    bool keyPressed(const juce::KeyPress& key) override {
        // Dialog windows receive their own keys: Tab here must bring rings back.
        if (AVDexedTheme::noteKeyboardNavigation(key)) repaint();
        return juce::AlertWindow::keyPressed(key);
    }
private:
    /** Dims the editor and keeps covering it if the editor is resized. */
    struct Veil final : public juce::Component, private juce::ComponentListener {
        explicit Veil(juce::Component& editorToCover) : host(&editorToCover) {
            setInterceptsMouseClicks(true, true);
            editorToCover.addChildComponent(*this);  // shown by fadeIn()
            setBounds(editorToCover.getLocalBounds());
            editorToCover.addComponentListener(this);
        }
        ~Veil() override { if (host != nullptr) host->removeComponentListener(this); }
        void paint(juce::Graphics& g) override { g.fillAll(juce::Colours::black.withAlpha(0.55f)); }
        void componentMovedOrResized(juce::Component& component, bool, bool) override {
            setBounds(component.getLocalBounds());
        }
        juce::Component::SafePointer<juce::Component> host;
    };
    DXLookNFeel AVLookAndFeel;
    std::unique_ptr<Veil> AVVeil;
};

/** Asynchronous AV-themed notice with a localized OK button. JUCE's static
    message boxes fall back to the host-wide LookAndFeel and an English "OK". */
void AVShowMessage(juce::MessageBoxIconType icon, const juce::String& title,
                   const juce::String& message, juce::Component* parent = nullptr);

#endif  // DXLOOKNFEEL_H_INCLUDED
