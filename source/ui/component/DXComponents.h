/**
 *
 * Copyright (c) 2014-2024 Pascal Gauthier.
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

#ifndef DXCOMPONENTS_H_INCLUDED
#define DXCOMPONENTS_H_INCLUDED

#include "../JuceLibraryCode/JuceHeader.h"
#include <stdint.h>

//==============================================================================
// THIS IS A TEMPORY FIX FOR COMBOBOX ACCESSIBILITY
// SEE: https://forum.juce.com/t/bug-combo-box-accessibility-bug/62501/2
// WILL BE REMOVED ONCE WE UPDATE TO JUCE 8
class ComboBoxAccessibilityHandlerFix final : public AccessibilityHandler
{
public:
    explicit ComboBoxAccessibilityHandlerFix (ComboBox& comboBoxToWrap)
        : AccessibilityHandler (comboBoxToWrap,
                                AccessibilityRole::comboBox,
                                getAccessibilityActions (comboBoxToWrap),
                                { std::make_unique<ComboBoxValueInterface> (comboBoxToWrap) }),
          comboBox (comboBoxToWrap)
    {
    }

    AccessibleState getCurrentState() const override
    {
        auto state = AccessibilityHandler::getCurrentState().withExpandable();

        return comboBox.isPopupActive() ? state.withExpanded() : state.withCollapsed();
    }

    String getTitle() const override  { return comboBox.getTitle(); }  // THE FIX IS RIGHT HERE
    String getHelp() const override   { return comboBox.getTooltip(); }

private:
    class ComboBoxValueInterface final : public AccessibilityTextValueInterface
    {
    public:
        explicit ComboBoxValueInterface (ComboBox& comboBoxToWrap)
            : comboBox (comboBoxToWrap)
        {
        }

        bool isReadOnly() const override                 { return true; }
        String getCurrentValueAsString() const override  { return comboBox.getText(); }
        void setValueAsString (const String&) override   {}

    private:
        ComboBox& comboBox;

        //==============================================================================
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ComboBoxValueInterface)
    };

    static AccessibilityActions getAccessibilityActions (ComboBox& comboBox)
    {
        return AccessibilityActions().addAction (AccessibilityActionType::press,    [&comboBox] { comboBox.showPopup(); })
                                     .addAction (AccessibilityActionType::showMenu, [&comboBox] { comboBox.showPopup(); });
    }

    ComboBox& comboBox;

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ComboBoxAccessibilityHandlerFix)
};
//==============================================================================

/** Up / Down belong to the editor everywhere (previous / next sound): a focused
    control lets them through instead of changing its value or selection. */
inline bool AVIsSoundStepKey(const KeyPress& key) {
    return key.isKeyCode(KeyPress::upKey) || key.isKeyCode(KeyPress::downKey);
}

class VectorChoiceBox : public ComboBox, private juce::ChangeListener {
public:
    enum class Preview { waveform, leftScale, rightScale };
    explicit VectorChoiceBox(Preview preview);
    ~VectorChoiceBox() override;

    void paint(Graphics &g) override;
    void resized() override;
    /** Left/Right still step the choice; Up/Down go to the editor (sounds). */
    bool keyPressed(const KeyPress& key) override { return ! AVIsSoundStepKey(key) && ComboBox::keyPressed(key); }

    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<ComboBoxAccessibilityHandlerFix> (*this);
    }
private:
    Preview previewType;
    void AVRefreshLanguage();
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
};

class CurveSelector final : public VectorChoiceBox {
public:
    explicit CurveSelector(bool left) : VectorChoiceBox(left ? Preview::leftScale : Preview::rightScale) {}
};

class WaveformSelector final : public VectorChoiceBox {
public:
    WaveformSelector() : VectorChoiceBox(Preview::waveform) {}
};

class ProgramSelector : public ComboBox {
    float accum_wheel = 0.0f;
public:
    ProgramSelector() {
        setWantsKeyboardFocus(true);
        setTitle("Program selector");
    }
    virtual void mouseDown(const MouseEvent &event) override;
    virtual void mouseEnter(const MouseEvent &event) override { accum_wheel = 0; }
    virtual void mouseWheelMove(const MouseEvent &event, const MouseWheelDetails &wheel) override;
    virtual void paint(Graphics &g) override;
    /** Left/Right still step the program; Up/Down go to the editor, which walks
        the sound list shown in Play (the same previous / next everywhere). */
    bool keyPressed(const KeyPress& key) override { return ! AVIsSoundStepKey(key) && ComboBox::keyPressed(key); }

    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<ComboBoxAccessibilityHandlerFix> (*this);
    }
};

// Knob that leaves Up/Down to the editor (previous / next sound). Left/Right
// step its value; with Shift they jump by 10 % of the range. The mouse wheel
// is finer with Shift.
class DXSlider : public Slider {
public:
    DXSlider(const String& componentName) : Slider(componentName) {
        setWantsKeyboardFocus(true);
        setVelocityModeParameters(1.0, 1, 0.0, true, ModifierKeys::shiftModifier);
    }

    bool keyPressed(const KeyPress &key) override {
        if (AVIsSoundStepKey(key))
            return false;
        const bool left = key.isKeyCode(KeyPress::leftKey), right = key.isKeyCode(KeyPress::rightKey);
        if ((left || right) && key.getModifiers().isShiftDown()) {
            const double jump = getRange().getLength() * 0.10;
            setValue(getValue() + (right ? jump : -jump));
            return true;
        }
        return Slider::keyPressed(key);
    }

    void mouseWheelMove(const MouseEvent &event, const MouseWheelDetails &wheel) override {
        auto newWheel = wheel;
        const auto speed = event.mods.isShiftDown() ? 0.05 : 1.0;
        newWheel.deltaY *= speed;
        Slider::mouseWheelMove(event, newWheel);
    }
};

class FocusLogger final : public juce::FocusChangeListener
{
public:
    FocusLogger ()
    {
        juce::Desktop::getInstance ().addFocusChangeListener (this);
    }

    ~FocusLogger () override
    {
        juce::Desktop::getInstance ().removeFocusChangeListener (this);
    }

    void globalFocusChanged (juce::Component * focusedComponent) override
    {
        if (focusedComponent == nullptr)
            return;

        DBG ("Component title: " << focusedComponent->getTitle ());
        DBG ("Component type: " << typeid (*focusedComponent).name ());
        DBG ("---");
    }
};

#endif  // DXCOMPONENTS_H_INCLUDED
