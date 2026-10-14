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
 */

#ifndef PLUGINEDITOR_H_INCLUDED
#define PLUGINEDITOR_H_INCLUDED

#include "../JuceLibraryCode/JuceHeader.h"
#include "PluginProcessor.h"
#include "OperatorEditor.h"
#include "GlobalEditor.h"
#include "component/DXComponents.h"
#include "DXLookNFeel.h"
#include "window/CartManager.h"
#include "component/ParameterObserver.h"
#include "AVDexedPlayView.h"
#include "AVDexedKeyboard.h"
#include "AVDexedResizeGrip.h"
#include "AVDexedWheel.h"
#include "AVDexedPeakMeter.h"

class AVDexedPanicButton final : public juce::TextButton {
public:
    AVDexedPanicButton();
    void setLabel(const juce::String& text);
    void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;

private:
    std::unique_ptr<juce::Drawable> icon;
    juce::String label;
};

//==============================================================================
/**
*/
class DexedAudioProcessorEditor  : public AudioProcessorEditor, public ComboBox::Listener, public Timer,
                                   public FileDragAndDropTarget, public KeyListener,
                                   private ChangeListener {
    // Declared first so all child components/dialogs release their LAF before it dies.
    SharedResourcePointer<DXLookNFeel> lookAndFeel;
    Component frameComponent;
    Component classicView;
    AVDexedKeyboardComponent midiKeyboard;
    AVDexedResizeGrip resizeGrip;
    AVDexedPlayView playView;
    Label brandLabel;
    std::unique_ptr<juce::Drawable> AVBrandSymbol;
    AVDexedWheel AVPitchWheel, AVModWheel;
    Label AVPitchLabel, AVModLabel, AVOctaveLabel;
    static constexpr int AVKeyboardAnchor = 24;
    Label AVHelpLabel, AVCpuLabel, AVVoicesLabel;
    AVDexedPeakMeter AVOutputMeter;
    double AVLastMeterUpdate = 0.0;
    DrawableButton AVSettingsButton { "AVSettings", DrawableButton::ImageFitted };
    std::unique_ptr<Drawable> AVInfoSymbol;
    void AVUpdateFooter();
    void AVLayoutFrame();
    void AVUpdateKeyboardRange();
    Label identityLabel;
    Label activePresetLabel;
    Label modifiedLabel;
    TextButton playTabButton { "Play" };
    TextButton advancedTabButton { "Advanced" };
    DrawableButton saveButton { "Save", DrawableButton::ImageFitted };
    TextButton languageButton { "ES" };
    AVDexedPanicButton panicButton;

    GlobalEditor global;
    std::unique_ptr<OperatorEditor> operators[6];
    Colour background;

    CartManager cartManager;

    // This cover is used to disable main window when cart manager is shown
    Component cartManagerCover;

    std::unique_ptr<juce::Component> AVSettingsOverlay;
    std::unique_ptr<FileChooser> fileChooser;
    int labelRefreshTicks = 0;
    int AVStatusTicks = 0;
    bool AVOperatorsSilent = false;
    float AVLastGlobalVu = -1.0f;
    int viewTransitionGeneration = 0;
    int resizeSaveCountdown = 0;
    float resizeFactorAtStart = 1.0f;
    void resetSize();
    void resized() override;
    void showAVDexedPlayView(bool showPlay, bool animate = true);
    void refreshPresetHeader();
    void applyLanguage();
    void changeListenerCallback(ChangeBroadcaster*) override;
    void requestPresetReplacement(std::function<void()> replacement);
    void commitCart(Cartridge cart, const File& file);
    void applyCart(Cartridge cart, File file);
    void storeProgramShowDialog(Cartridge destSysex, File externalFile);
    void storeProgramResponse(AlertWindow *dialog, int response, Cartridge destSysex, File externalFile, bool hasExternalFile);
    void saveNativePreset(bool saveAs, std::function<void(bool)> completion = {});
    void loadNativePreset(const File& file);
public:
    DexedAudioProcessor *processor;

    DexedAudioProcessorEditor (DexedAudioProcessor* ownerFilter);
    ~DexedAudioProcessorEditor();
    virtual void timerCallback() override;

    virtual void paint (Graphics& g) override;
    virtual void comboBoxChanged (ComboBox* comboBoxThatHasChanged) override;
    void updateUI();
    void rebuildProgramCombobox();
    void loadCart(File file);
    void saveCart();
    void initProgram();
    void storeProgram();
    void cartShow();
    void parmShow();
    void tuningShow();

    static float getLargestScaleFactor();
    void resetZoomFactor();

    virtual bool isInterestedInFileDrag (const StringArray &files) override;
    virtual void filesDropped (const StringArray &files, int x, int y ) override;
    std::unique_ptr<ComponentTraverser> createFocusTraverser() override;

    bool keyPressed(const KeyPress& key, Component* originatingComponent) override;

    /** A mouse click hides focus rings until the next keyboard navigation. */
    struct AVFocusModality final : public MouseListener {
        void mouseDown(const MouseEvent& event) override;
    } focusModality;

    static constexpr float AVTopBarAxis = 47.0f;  // shared centre line of the top bar
    static const int WINDOW_SIZE_X = 1440;
    static const int WINDOW_SIZE_Y = 940;

#if defined(AVDEXED_TESTING)
    friend bool AVRenderUIEvidence(const juce::File& directory);
#endif

private:
    friend class CartManager;
#ifdef DEXED_EVENT_DEBUG
    FocusLogger focusLogger;
#endif
};


#endif  // PLUGINEDITOR_H_INCLUDED
