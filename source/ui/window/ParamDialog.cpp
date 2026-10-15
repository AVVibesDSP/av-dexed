/*
  ==============================================================================

  This is an automatically generated GUI class created by the Projucer!

  Be careful when adding custom code to these files, as only the code within
  the "//[xyz]" and "//[/xyz]" sections will be retained when the file is loaded
  and re-saved.

  Created with Projucer version: 7.0.11

  ------------------------------------------------------------------------------

  The Projucer is part of the JUCE library.
  Copyright (c) 2020 - Raw Material Software Limited.

  ==============================================================================
*/

//[Headers] You can add your own extra header files here...
#include "../../core/AVDexedLibrary.h"
#include "../../debugger/Logger.h"
#include "../component/DXComponents.h"
#include "parameter/Model.h"
//[/Headers]

#include "../PluginEditor.h"
#include "ParamDialog.h"
#include "../AVDexedTheme.h"
#include <cmath>

//[MiscUserDefs] You can add your own user definitions and misc code here...
//[/MiscUserDefs]

//==============================================================================
ParamDialog::ParamDialog (DexedApvts &apvts, DexedAudioProcessor &proc) : apvts_(apvts), proc_(proc), attachments_(*this, apvts) {
    //[Constructor_pre] You can add your own custom stuff here..
    //[/Constructor_pre]

    pitchRangeDn.reset(new juce::Slider(IDs::pitchRangeDown.name));
    addAndMakeVisible(pitchRangeDn.get());
    pitchRangeDn->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchRangeDn->setTextBoxStyle(juce::Slider::TextBoxLeft, false, 80, 20);
    pitchRangeDn->addListener(this);
    pitchRangeDn->setBounds(264, 16, 72, 24);
    attachments_.attach(pitchRangeDn.get());

    pitchStep.reset(new juce::Slider(IDs::pitchStep.name));
    addAndMakeVisible(pitchStep.get());
    pitchStep->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchStep->setTextBoxStyle(juce::Slider::TextBoxLeft, false, 80, 20);
    pitchStep->addListener(this);
    pitchStep->setBounds(264, 56, 72, 24);
    attachments_.attach(pitchStep.get());

    sysexIn.reset (new juce::ComboBox ("sysexIn"));
    addAndMakeVisible (sysexIn.get());
    sysexIn->setExplicitFocusOrder (7);
    sysexIn->setEditableText (false);
    sysexIn->setJustificationType (juce::Justification::centredLeft);
    sysexIn->setTextWhenNothingSelected (juce::String());
    sysexIn->setTextWhenNoChoicesAvailable (AVDexedLanguageService::getInstance().text("noChoices"));
    sysexIn->addListener (this);

    sysexIn->setBounds (104, 244, 224, 24);

    sysexChl.reset (new juce::Slider ("sysexChl"));
    addAndMakeVisible (sysexChl.get());
    sysexChl->setExplicitFocusOrder (9);
    sysexChl->setRange (1, 16, 1);
    sysexChl->setSliderStyle (juce::Slider::RotaryVerticalDrag);
    sysexChl->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 20);
    sysexChl->addListener (this);

    sysexChl->setBounds (252, 320, 72, 24);

    engineType.reset(new juce::ComboBox(IDs::engineType.name));
    addAndMakeVisible(engineType.get());
    engineType->setJustificationType(juce::Justification::centredLeft);
    engineType->setBounds(160, 188, 168, 24);
    attachments_.attach(engineType.get());

    showKeyboard.reset (new LightedToggleButton ("showKeyboard"));
    addAndMakeVisible (showKeyboard.get());
    showKeyboard->setExplicitFocusOrder (4);
    showKeyboard->setButtonText (juce::String());
    showKeyboard->addListener (this);

    showKeyboard->setBounds (264, 96, 56, 24);

    // Wheel modulation components (owned by attachments_)
    auto whlRange = std::make_unique<juce::Slider>(IDs::modWheel.name);
    whlRange->setSliderStyle (juce::Slider::RotaryVerticalDrag);
    whlRange->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 20);
    whlRange->setBounds (448, 16, 72, 24);
    whlRange->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(whlRange));

    auto whlPitch = std::make_unique<LightedToggleButton>(IDs::modWheelPitch.name);
    whlPitch->setBounds (528, 16, 56, 24);
    whlPitch->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(whlPitch));

    auto whlAmp = std::make_unique<LightedToggleButton>(IDs::modWheelAmp.name);
    whlAmp->setBounds (584, 16, 56, 24);
    whlAmp->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(whlAmp));

    auto whlEg = std::make_unique<LightedToggleButton>(IDs::modWheelEgBias.name);
    whlEg->setBounds (640, 16, 56, 24);
    whlEg->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(whlEg));

    // Foot modulation components (owned by attachments_)
    auto ftRange = std::make_unique<juce::Slider>(IDs::modFoot.name);
    ftRange->setSliderStyle (juce::Slider::RotaryVerticalDrag);
    ftRange->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 20);
    ftRange->setBounds (448, 56, 72, 24);
    ftRange->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(ftRange));

    auto ftPitch = std::make_unique<LightedToggleButton>(IDs::modFootPitch.name);
    ftPitch->setBounds (528, 56, 56, 24);
    ftPitch->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(ftPitch));

    auto ftAmp = std::make_unique<LightedToggleButton>(IDs::modFootAmp.name);
    ftAmp->setBounds (584, 56, 56, 24);
    ftAmp->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(ftAmp));

    auto ftEg = std::make_unique<LightedToggleButton>(IDs::modFootEgBias.name);
    ftEg->setBounds (640, 56, 56, 24);
    ftEg->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(ftEg));

    // Breath modulation components (owned by attachments_)
    auto brRange = std::make_unique<juce::Slider>(IDs::modBreath.name);
    brRange->setSliderStyle (juce::Slider::RotaryVerticalDrag);
    brRange->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 20);
    brRange->setBounds (448, 96, 72, 24);
    brRange->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(brRange));

    auto brPitch = std::make_unique<LightedToggleButton>(IDs::modBreathPitch.name);
    brPitch->setBounds (528, 96, 56, 24);
    brPitch->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(brPitch));

    auto brAmp = std::make_unique<LightedToggleButton>(IDs::modBreathAmp.name);
    brAmp->setBounds (584, 96, 56, 24);
    brAmp->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(brAmp));

    auto brEg = std::make_unique<LightedToggleButton>(IDs::modBreathEgBias.name);
    brEg->setBounds (640, 96, 56, 24);
    brEg->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(brEg));

    // Aftertouch modulation components (owned by attachments_)
    auto atRange = std::make_unique<juce::Slider>(IDs::modAftertouch.name);
    atRange->setSliderStyle (juce::Slider::RotaryVerticalDrag);
    atRange->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 20);
    atRange->setBounds (448, 136, 72, 24);
    atRange->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(atRange));

    auto atPitch = std::make_unique<LightedToggleButton>(IDs::modAftertouchPitch.name);
    atPitch->setBounds (528, 136, 56, 24);
    atPitch->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(atPitch));

    auto atAmp = std::make_unique<LightedToggleButton>(IDs::modAftertouchAmp.name);
    atAmp->setBounds (584, 136, 56, 24);
    atAmp->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(atAmp));

    auto atEg = std::make_unique<LightedToggleButton>(IDs::modAftertouchEgBias.name);
    atEg->setBounds (640, 136, 56, 24);
    atEg->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(atEg));

    sclButton.reset (new juce::TextButton ("scl button"));
    addAndMakeVisible (sclButton.get());
    sclButton->setExplicitFocusOrder (26);
    sclButton->setButtonText ("SCL");
    sclButton->addListener (this);

    sclButton->setBounds (448, 205, 56, 30);

    kbmButton.reset (new juce::TextButton ("kbm button"));
    addAndMakeVisible (kbmButton.get());
    kbmButton->setExplicitFocusOrder (27);
    kbmButton->setButtonText ("KBM");
    kbmButton->addListener (this);

    kbmButton->setBounds (512, 205, 56, 30);

    showTunButton.reset (new juce::TextButton ("show tuning button"));
    addAndMakeVisible (showTunButton.get());
    showTunButton->setExplicitFocusOrder (28);
    showTunButton->setButtonText (AVDexedLanguageService::getInstance().text("show"));
    showTunButton->addListener (this);

    showTunButton->setBounds (576, 205, 48, 30);

    resetTuningButton.reset (new juce::TextButton ("reset tuning button"));
    addAndMakeVisible (resetTuningButton.get());
    resetTuningButton->setExplicitFocusOrder (29);
    resetTuningButton->setButtonText (AVDexedLanguageService::getInstance().text("reset"));
    resetTuningButton->addListener (this);

    resetTuningButton->setBounds (632, 205, 48, 30);

    auto transposeScale = std::make_unique<LightedToggleButton>(IDs::transposeTuningScale.name);
    transposeScale->setBounds(576, 240, 56, 30);
    transposeScale->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(transposeScale));

    mpePBRange.reset (new juce::Slider ("mpePBRange"));
    addAndMakeVisible (mpePBRange.get());
    mpePBRange->setExplicitFocusOrder (32);
    mpePBRange->setRange (0, 96, 1);
    mpePBRange->setSliderStyle (juce::Slider::RotaryVerticalDrag);
    mpePBRange->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 20);
    mpePBRange->addListener (this);

    mpePBRange->setBounds (616, 319, 72, 24);

    mpeEnabled.reset (new LightedToggleButton ("mpeEnabled"));
    addAndMakeVisible (mpeEnabled.get());
    mpeEnabled->setExplicitFocusOrder (31);
    mpeEnabled->setButtonText (juce::String());
    mpeEnabled->addListener (this);

    mpeEnabled->setBounds (456, 320, 48, 30);

    transposeHelp.reset (new juce::TextButton ("?"));
    addAndMakeVisible (transposeHelp.get());
    transposeHelp->addListener (this);
    transposeHelp->setTooltip (AVDexedLanguageService::getInstance().text("transposeScaleHelp"));
    transposeHelp->setBounds (500, 245, 20, 20);

    pitchRangeUp.reset(new juce::Slider(IDs::pitchRangeUp.name));
    addAndMakeVisible(pitchRangeUp.get());
    pitchRangeUp->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchRangeUp->setTextBoxStyle(juce::Slider::TextBoxLeft, false, 80, 20);
    pitchRangeUp->addListener(this);
    pitchRangeUp->setBounds(168, 16, 72, 24);
    attachments_.attach(pitchRangeUp.get());

    scalingFactor.reset (new juce::ComboBox ("scalingFactor"));
    addAndMakeVisible (scalingFactor.get());
    scalingFactor->setExplicitFocusOrder (5);
    scalingFactor->setEditableText (false);
    scalingFactor->setJustificationType (juce::Justification::centredLeft);
    scalingFactor->setTextWhenNothingSelected (juce::String());
    scalingFactor->setTextWhenNoChoicesAvailable (AVDexedLanguageService::getInstance().text("noChoices"));
#if JUCE_WINDOWS
    // Windows allows 60 % (AVMinZoom) for 1080p screens at 150 % scaling.
    scalingFactor->addItem("60 %", 8);
    scalingFactor->addItem("70 %", 9);
#endif
    scalingFactor->addItem("80 %", 1);
    scalingFactor->addItem("90 %", 2);
    scalingFactor->addItem("100 %", 3);
    scalingFactor->addItem("110 %", 4);
    scalingFactor->addItem("125 %", 5);
    scalingFactor->addItem("150 %", 6);
    scalingFactor->addItem("160 %", 7);
    scalingFactor->addListener (this);

    scalingFactor->setBounds (236, 136, 90, 24);

    auto glissando = std::make_unique<LightedToggleButton>(IDs::glissando.name);
    glissando->setBounds(576, 276, 56, 30);
    glissando->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(glissando));

    auto portamentoTm = std::make_unique<juce::Slider>(IDs::portamentoTm.name);
    portamentoTm->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    portamentoTm->setTextBoxStyle(juce::Slider::TextBoxLeft, false, 80, 20);
    portamentoTm->setBounds(504, 276, 72, 24);
    portamentoTm->setWantsKeyboardFocus(true);
    attachments_.addAndAttach(std::move(portamentoTm));


    profileSelector.reset(new juce::ComboBox("profileSelector"));
    addAndMakeVisible(profileSelector.get());
    profileSelector->addListener(this);
    profileSelector->setBounds(100, 378, 450, 24);

    storeButton.reset(new juce::TextButton("storeButton"));
    addAndMakeVisible(storeButton.get());
    storeButton->setButtonText("Store");
    storeButton->addListener(this);
    storeButton->setBounds(558, 378, 130, 24);



    setLookAndFeel(&AVLookAndFeel);
    setSize(860, 540);


    //[Constructor] You can add your own custom stuff here..
    pitchRangeUp->setEnabled(pitchStep->getValue() == 0);
    pitchRangeDn->setEnabled(pitchStep->getValue() == 0);

    sysexIn->addItem(AVDexedLanguageService::getInstance().text("none"), 1);
    for (const auto& device : MidiInput::getAvailableDevices())
        sysexIn->addItem(device.name, sysexIn->getNumItems() + 1);
    sysexIn->setSelectedId(1, dontSendNotification);

    // SysEx port and channel are edited in Settings > SysEx; these stay hidden
    // and only mirror the current values (setDialogValues) when read back.
    sysexIn->setVisible(false);
    sysexChl->setVisible(false);

    // ACCESSIBLITY (modulation component titles set by AudioComponentContainer::attach)
    pitchRangeUp->setTitle("Pitch Bend Range Up");
    pitchRangeDn->setTitle("Pitch Bend Range Down");
    pitchStep->setTitle("Pitch Bend Step");
    sysexIn->setTitle("Sysex Input");
    sysexChl->setTitle("Sysex Channel");
    engineType->setTitle("Engine Resolution");
    showKeyboard->setTitle("Show Keyboard");
    sclButton->setTitle("Scale Button");
    kbmButton->setTitle("Keyboard Mapping Button");
    showTunButton->setTitle("Show Tuning Button");
    resetTuningButton->setTitle("Reset Tuning Button");
    mpePBRange->setTitle("MPE Pitch Bend Range");
    mpeEnabled->setTitle("MPE Enabled");
    transposeHelp->setTitle("Transpose Help");
    scalingFactor->setTitle("Scaling Factor");
    profileSelector->setTitle("Profile");
    storeButton->setTitle("Store Profile");

    pitchRangeUp->setWantsKeyboardFocus(true);
    pitchRangeDn->setWantsKeyboardFocus(true);
    pitchStep->setWantsKeyboardFocus(true);
    sysexIn->setWantsKeyboardFocus(true);
    sysexChl->setWantsKeyboardFocus(true);
    engineType->setWantsKeyboardFocus(true);
    showKeyboard->setWantsKeyboardFocus(true);
    sclButton->setWantsKeyboardFocus(true);
    kbmButton->setWantsKeyboardFocus(true);
    showTunButton->setWantsKeyboardFocus(true);
    resetTuningButton->setWantsKeyboardFocus(true);
    mpePBRange->setWantsKeyboardFocus(true);
    mpeEnabled->setWantsKeyboardFocus(true);
    transposeHelp->setWantsKeyboardFocus(true);
    scalingFactor->setWantsKeyboardFocus(true);
    profileSelector->setWantsKeyboardFocus(true);
    storeButton->setWantsKeyboardFocus(true);

    for (auto* child : getChildren()) {
        child->setWantsKeyboardFocus(true);
        if (auto* slider = dynamic_cast<Slider*>(child)) {
            slider->setSliderStyle(Slider::LinearHorizontal);
            slider->setTextBoxStyle(Slider::TextBoxLeft, false, int(AVDexedSettingsStyle::valueWidth), 28);
            slider->setVelocityModeParameters(1.0, 1, 0.0, true, ModifierKeys::shiftModifier);
        }
    }
    // GOZO settings: On/Off is a pair of pills (the toggles stay, hidden, so
    // their attachments and listeners are untouched); the modulation matrix
    // uses ring toggles.
    const auto pills = [this](Component* toggle) {
        auto* button = dynamic_cast<Button*>(toggle);
        jassert(button != nullptr);
        button->setVisible(false);
        auto pair = std::make_unique<AVDexedSettingsStyle::OnOffPills>(*button);
        addAndMakeVisible(*pair);
        return pair;
    };
    AVGlissandoPills = pills(AVChild(IDs::glissando.name));
    AVShowKeyboardPills = pills(showKeyboard.get());
    AVTransposePills = pills(AVChild(IDs::transposeTuningScale.name));
    AVMpePills = pills(mpeEnabled.get());
    for (const auto& name : { IDs::modWheelPitch.name, IDs::modWheelAmp.name, IDs::modWheelEgBias.name,
                              IDs::modFootPitch.name, IDs::modFootAmp.name, IDs::modFootEgBias.name,
                              IDs::modBreathPitch.name, IDs::modBreathAmp.name, IDs::modBreathEgBias.name,
                              IDs::modAftertouchPitch.name, IDs::modAftertouchAmp.name, IDs::modAftertouchEgBias.name })
        if (auto* ring = AVChild(name)) ring->getProperties().set("AVRing", true);
    AVDexedLanguageService::getInstance().addChangeListener(this);
    AVRefreshLanguage();
    resized();
    setWantsKeyboardFocus(true);
    startTimer(100);
    //[/Constructor]
}

ParamDialog::~ParamDialog() {
    //[Destructor_pre]. You can add your own custom destruction code here..
    AVDexedLanguageService::getInstance().removeChangeListener(this);
    setLookAndFeel(nullptr);
    attachments_.freeAttachments();
    //[/Destructor_pre]

    pitchRangeDn = nullptr;
    pitchStep = nullptr;
    sysexIn = nullptr;
    sysexChl = nullptr;
    engineType = nullptr;
    showKeyboard = nullptr;
    sclButton = nullptr;
    kbmButton = nullptr;
    showTunButton = nullptr;
    resetTuningButton = nullptr;
    mpePBRange = nullptr;
    mpeEnabled = nullptr;
    transposeHelp = nullptr;
    pitchRangeUp = nullptr;
    scalingFactor = nullptr;
    profileSelector = nullptr;
    storeButton = nullptr;


    //[Destructor]. You can add your own custom destruction code here..
    stopTimer();
    //[/Destructor]
}

//==============================================================================
juce::Component* ParamDialog::AVChild(const juce::String& name) const {
    for (auto* child : getChildren())
        if (child->getName() == name) return child;
    return nullptr;
}

// GOZO settings layout in two columns (each: labels right-aligned in 0..170,
// fields from 182), sections under a heading with a hairline. No cards.
void ParamDialog::paint(Graphics& g) {
    namespace S = AVDexedSettingsStyle;
    auto& language = AVDexedLanguageService::getInstance();
    const float column = float(AVColumnWidth()), right = column + 35.0f;
    const auto row = [](int index) { return 34.0f + 36.0f * float(index); };

    S::heading(g, language.text("instrumentGeneral"), { 0.0f, 0.0f, column, 24.0f });
    const char* leftKeys[] { "pitchBendUp", "pitchBendDown", "pitchBendStep", "portamentoRate", "glissando",
                             "showKeyboard", "uiScaling", "engineResolution" };
    for (int i = 0; i < 8; ++i)
        S::label(g, language.text(leftKeys[i]) + ":", { 0.0f, row(i), 170.0f, S::rowHeight });
    S::heading(g, language.text("profile"), { 0.0f, 340.0f, column, 24.0f });

    S::heading(g, language.text("controllerModulation"), { right, 0.0f, column, 24.0f });
    S::hint(g, language.text("modRange"), { right + 134.0f, 28.0f, 120.0f, 20.0f });
    const char* columns[] { "modPitch", "modAmplitude", "modEgBias" };
    for (int i = 0; i < 3; ++i)
        S::hint(g, language.text(columns[i]), { right + 274.0f + 42.0f * float(i), 28.0f, 42.0f, 20.0f }, Justification::centred);
    const char* sources[] { "wheel", "foot", "breath", "aftertouch" };
    for (int i = 0; i < 4; ++i)
        S::label(g, language.text(sources[i]), { right, 52.0f + 32.0f * float(i), 130.0f, 28.0f }, Justification::centredLeft);

    S::heading(g, language.text("tuning"), { right, 200.0f, column, 24.0f });
    S::label(g, language.text("transposeScale") + ":", { right, 270.0f, 170.0f, S::rowHeight });
    S::heading(g, language.text("mpeSection"), { right, 320.0f, column, 24.0f });
    S::label(g, language.text("mpeEnabled") + ":", { right, 354.0f, 170.0f, S::rowHeight });
    S::label(g, language.text("mpeBendRange") + ":", { right, 390.0f, 170.0f, S::rowHeight });
}

void ParamDialog::resized() {
    if (AVMpePills == nullptr) return;   // setSize() in the constructor runs before the pills exist
    const int column = AVColumnWidth(), right = column + 35, field = 182, fieldWidth = column - field;
    const auto row = [](int index) { return 34 + 36 * index; };
    const auto place = [this](const String& name, Rectangle<int> bounds) {
        if (auto* child = AVChild(name)) child->setBounds(bounds);
    };
    const auto pillsWidth = [](AVDexedSettingsStyle::OnOffPills& pair, int available) {
        return jmin(available, pair.idealWidth());
    };

    pitchRangeUp->setBounds(field, row(0), fieldWidth, 30);
    pitchRangeDn->setBounds(field, row(1), fieldWidth, 30);
    pitchStep->setBounds(field, row(2), fieldWidth, 30);
    place(IDs::portamentoTm.name, { field, row(3), fieldWidth, 30 });
    AVGlissandoPills->setBounds(field, row(4), pillsWidth(*AVGlissandoPills, fieldWidth), 30);
    AVShowKeyboardPills->setBounds(field, row(5), pillsWidth(*AVShowKeyboardPills, fieldWidth), 30);
    scalingFactor->setBounds(field, row(6), fieldWidth, 30);
    engineType->setBounds(field, row(7), fieldWidth, 30);
    sysexIn->setBounds(field, row(7), fieldWidth, 30);     // hidden: lives in Settings > SysEx
    sysexChl->setBounds(field, row(7), fieldWidth, 30);

    const int save = AVDexedSettingsStyle::pillWidth(storeButton->getButtonText(), 110);
    profileSelector->setBounds(0, 374, column - save - 8, 30);
    storeButton->setBounds(column - save, 374, save, 30);

    const String ranges[] { IDs::modWheel.name, IDs::modFoot.name, IDs::modBreath.name, IDs::modAftertouch.name };
    const String pitch[] { IDs::modWheelPitch.name, IDs::modFootPitch.name, IDs::modBreathPitch.name, IDs::modAftertouchPitch.name };
    const String amplitude[] { IDs::modWheelAmp.name, IDs::modFootAmp.name, IDs::modBreathAmp.name, IDs::modAftertouchAmp.name };
    const String bias[] { IDs::modWheelEgBias.name, IDs::modFootEgBias.name, IDs::modBreathEgBias.name, IDs::modAftertouchEgBias.name };
    for (int index = 0; index < 4; ++index) {
        const int y = 52 + 32 * index;
        place(ranges[index], { right + 134, y, 132, 28 });
        place(pitch[index], { right + 274, y, 42, 28 });
        place(amplitude[index], { right + 316, y, 42, 28 });
        place(bias[index], { right + 358, y, 42, 28 });
    }

    int x = right;
    for (auto* button : { sclButton.get(), kbmButton.get(), showTunButton.get(), resetTuningButton.get() }) {
        const int width = AVDexedSettingsStyle::pillWidth(button->getButtonText(), 64);
        button->setBounds(x, 234, width, 30);
        x += width + 8;
    }
    transposeHelp->setBounds(right + column - 30, 234, 30, 30);   // "?" explains transposing by scale
    AVTransposePills->setBounds(right + field, 270, pillsWidth(*AVTransposePills, fieldWidth), 30);
    AVMpePills->setBounds(right + field, 354, pillsWidth(*AVMpePills, fieldWidth), 30);
    mpePBRange->setBounds(right + field, 390, fieldWidth, 30);
}

void ParamDialog::changeListenerCallback(juce::ChangeBroadcaster*) { AVRefreshLanguage(); }

void ParamDialog::AVRefreshLanguage() {
    auto& language = AVDexedLanguageService::getInstance();
    const std::pair<Component*, const char*> controls[] {
        { pitchRangeUp.get(), "pitchBendUp" }, { pitchRangeDn.get(), "pitchBendDown" },
        { pitchStep.get(), "pitchBendStep" }, { sysexIn.get(), "sysexInput" }, { sysexChl.get(), "sysexChannel" },
        { engineType.get(), "engineResolution" }, { showKeyboard.get(), "showKeyboard" },
        { sclButton.get(), "loadScale" }, { kbmButton.get(), "loadKeyboardMap" },
        { showTunButton.get(), "showTuning" }, { resetTuningButton.get(), "resetTuning" },
        { mpePBRange.get(), "mpeBendRange" }, { mpeEnabled.get(), "mpeEnabled" },
        { transposeHelp.get(), "transposeScaleHelp" }, { scalingFactor.get(), "uiScaling" },
        { profileSelector.get(), "profile" }, { storeButton.get(), "storeProfile" }
    };
    for (const auto& item : controls) {
        item.first->setTitle(language.text(item.second));
        if (auto* tooltip = dynamic_cast<SettableTooltipClient*>(item.first))
            tooltip->setTooltip(language.text(item.second));
    }
    const String rangeIds[] { IDs::modWheel.name, IDs::modFoot.name, IDs::modBreath.name, IDs::modAftertouch.name };
    const char* rowKeys[] { "wheel", "foot", "breath", "aftertouch" };
    const String pitchIds[] { IDs::modWheelPitch.name, IDs::modFootPitch.name, IDs::modBreathPitch.name, IDs::modAftertouchPitch.name };
    const String ampIds[] { IDs::modWheelAmp.name, IDs::modFootAmp.name, IDs::modBreathAmp.name, IDs::modAftertouchAmp.name };
    const String biasIds[] { IDs::modWheelEgBias.name, IDs::modFootEgBias.name, IDs::modBreathEgBias.name, IDs::modAftertouchEgBias.name };
    for (int row = 0; row < 4; ++row)
        for (auto* child : getChildren()) {
            const auto id = child->getName();
            const auto suffix = id == rangeIds[row] ? "modRange" : id == pitchIds[row] ? "modPitch"
                              : id == ampIds[row] ? "modAmplitude" : id == biasIds[row] ? "modEgBias" : "";
            if (String(suffix).isNotEmpty()) {
                child->setTitle(language.text(rowKeys[row]) + String::fromUTF8(" · ") + language.text(suffix));
                if (auto* tooltip = dynamic_cast<SettableTooltipClient*>(child)) tooltip->setTooltip(child->getTitle());
            }
            if (id == IDs::transposeTuningScale.name) child->setTitle(language.text("transposeScale"));
            if (id == IDs::portamentoTm.name) child->setTitle(language.text("portamentoRate"));
            if (id == IDs::glissando.name) child->setTitle(language.text("glissando"));
        }
    for (auto* child : getChildren())
        if (auto* tooltip = dynamic_cast<SettableTooltipClient*>(child))
            tooltip->setTooltip(child->getTitle() + (dynamic_cast<Slider*>(child) != nullptr
                ? "\n" + language.text("sliderHelp") : String()));
    showTunButton->setButtonText(language.text("show"));
    resetTuningButton->setButtonText(language.text("reset"));
    storeButton->setButtonText(language.text("storeProfile"));
    for (auto* pair : { AVGlissandoPills.get(), AVShowKeyboardPills.get(), AVTransposePills.get(), AVMpePills.get() })
        if (pair != nullptr) pair->setTexts(language.text("optionOn"), language.text("optionOff"));
    resized();
    // changeItemText leaves the visible caption stale, and getSelectedId() reads
    // 0 once caption and item differ: keep the ids and refresh both displays.
    const int engineId = engineType->getSelectedId();
    const int sysexId = sysexIn->getSelectedId();
    engineType->changeItemText(1, language.text("engineModern"));
    sysexIn->changeItemText(1, language.text("none"));
    engineType->setSelectedId(engineId, dontSendNotification);
    sysexIn->setSelectedId(sysexId, dontSendNotification);
    sysexIn->setTextWhenNoChoicesAvailable(language.text("noChoices"));
    const auto profileId = profileSelector->getSelectedId();
    refreshProfileList();
    profileSelector->setSelectedId(jmax(1, profileId), dontSendNotification);
    repaint();
}

void ParamDialog::sliderValueChanged (juce::Slider* sliderThatWasMoved) {
    //[UsersliderValueChanged_Pre]
    bool handled = false;
    //[/UsersliderValueChanged_Pre]

    if (sliderThatWasMoved == pitchStep.get()) {
        //[UserSliderCode_pitchStep] -- add your slider handling code here..
        pitchRangeUp->setEnabled(pitchStep->getValue() == 0);
        pitchRangeDn->setEnabled(pitchStep->getValue() == 0);
        //[/UserSliderCode_pitchStep]
    }

    //[UsersliderValueChanged_Post]
    if( ! handled )
        general_callback_(this);
    //[/UsersliderValueChanged_Post]
}

void ParamDialog::comboBoxChanged (juce::ComboBox* comboBoxThatHasChanged) {
    //[UsercomboBoxChanged_Pre]
    bool handled = false;
    //[/UsercomboBoxChanged_Pre]

    if (comboBoxThatHasChanged == sysexIn.get()) {
        //[UserComboBoxCode_sysexIn] -- add your combo box handling code here..
        //[/UserComboBoxCode_sysexIn]
    }
    else if (comboBoxThatHasChanged == scalingFactor.get()) {
        //[UserComboBoxCode_scalingFactor] -- add your combo box handling code here..
        //[/UserComboBoxCode_scalingFactor]
    }

    else if (comboBoxThatHasChanged == profileSelector.get()) {
        int idx = profileSelector->getSelectedItemIndex();
        if (idx > 0) {
            juce::File profileDir = DexedAudioProcessor::dexedAppDir.getChildFile(AVDexedLibrary::profilesFolderName);
            juce::Array<juce::File> files;
            profileDir.findChildFiles(files, juce::File::findFiles, false, "*.xml");
            files.sort();
            int fileIdx = idx - 1;
            if (fileIdx < files.size())
                loadProfileFromFile(files[fileIdx]);
        }
        handled = true;
    }

    //[UsercomboBoxChanged_Post]
    if( ! handled )
        general_callback_(this);
    //[/UsercomboBoxChanged_Post]
}

void ParamDialog::buttonClicked (juce::Button* buttonThatWasClicked) {
    //[UserbuttonClicked_Pre]
    bool handled = false;
    //[/UserbuttonClicked_Pre]

    if (buttonThatWasClicked == showKeyboard.get()) {
        //[UserButtonCode_showKeyboard] -- add your button handler code here..
        //[/UserButtonCode_showKeyboard]
    }
    else if (buttonThatWasClicked == sclButton.get()) {
        //[UserButtonCode_sclButton] -- add your button handler code here..
        tuning_callback_(this, TuningAction::LOAD_SCL);
        handled = true;
        //[/UserButtonCode_sclButton]
    }
    else if (buttonThatWasClicked == kbmButton.get()) {
        //[UserButtonCode_kbmButton] -- add your button handler code here..
        tuning_callback_(this, TuningAction::LOAD_KBM);
        handled = true;
        //[/UserButtonCode_kbmButton]
    }
    else if (buttonThatWasClicked == showTunButton.get()) {
        //[UserButtonCode_showTunButton] -- add your button handler code here..
        tuning_callback_(this, TuningAction::SHOW_TUNING);
        handled = true;
        //[/UserButtonCode_showTunButton]
    }
    else if (buttonThatWasClicked == resetTuningButton.get()) {
        //[UserButtonCode_resetTuningButton] -- add your button handler code here..
        tuning_callback_(this, TuningAction::RESET_TUNING);
        handled = true;
        //[/UserButtonCode_resetTuningButton]
    }
    else if (buttonThatWasClicked == mpeEnabled.get()) {
        //[UserButtonCode_mpeEnabled] -- add your button handler code here..
        //[/UserButtonCode_mpeEnabled]
    }
    else if (buttonThatWasClicked == transposeHelp.get()) {
        //[UserButtonCode_transposeHelp] -- add your button handler code here..
        // An in-editor notice over Settings, like every other dialog (it was a
        // desktop window with its own title bar).
        auto& language = AVDexedLanguageService::getInstance();
        AVShowMessage(MessageBoxIconType::NoIcon, language.text("transposeHelpTitle"),
                      language.text("transposeHelpText"), this);
        //[/UserButtonCode_transposeHelp]
    }
    else if (buttonThatWasClicked == storeButton.get()) {
        AVAskProfileName();
        handled = true;
    }

    //[UserbuttonClicked_Post]
    if( ! handled ) {
        general_callback_(this);
    }
    //[/UserbuttonClicked_Post]
}



//[MiscUserCode] You can add your own definitions of your custom methods or any other code here...

void ParamDialog::setDialogValues(Controllers &c, SysexComm &mgr, bool showKey, float dpiScaleFactor) {
    sysexChl->setValue(mgr.getChl() + 1);

    // Modulation components (whl, ft, br, at) are synced via parameter attachments

    mpeEnabled->setToggleState(c.mpeEnabled, dontSendNotification);
    mpePBRange->setValue(c.mpePitchBendRange, dontSendNotification);

    int inputIndex = 0;
    for (int i = 1; i < sysexIn->getNumItems(); ++i)
        if (sysexIn->getItemText(i) == mgr.getInput()) inputIndex = i;
    sysexIn->setSelectedItemIndex(inputIndex, dontSendNotification);

    showKeyboard->setToggleState(showKey, NotificationType::dontSendNotification);

    const auto exactScale = String(dpiScaleFactor * 100.0f, 1).trimCharactersAtEnd("0").trimCharactersAtEnd(".") + " %";
    int selectedId = 0;
    for (int i = 0; i < scalingFactor->getNumItems(); ++i)
        if (approximatelyEqual(scalingFactor->getItemText(i).getFloatValue() / 100.0f, dpiScaleFactor))
            selectedId = scalingFactor->getItemId(i);
    if (selectedId == 0) {
        selectedId = 100;
        scalingFactor->addItem(exactScale, selectedId);
    }
    scalingFactor->setSelectedId(selectedId, dontSendNotification);
}

bool ParamDialog::getDialogValues(Controllers &c, SysexComm &mgr, bool *showKey, float *dpiScaleFactor) {
    bool ret = true;

    // Modulation components (whl, ft, br, at) are synced via parameter attachments

    c.mpeEnabled = mpeEnabled->getToggleState();
    c.mpePitchBendRange = mpePBRange->getValue();

    c.refresh();

    if ( ! JUCEApplication::isStandaloneApp() ) {
        ret &= mgr.setInput(sysexIn->getSelectedItemIndex() <= 0 ? String() : sysexIn->getItemText(sysexIn->getSelectedItemIndex()));
    }

    mgr.setChl(sysexChl->getValue() - 1);

    *showKey = showKeyboard->getToggleState();

    *dpiScaleFactor = jlimit(DexedAudioProcessorEditor::AVMinZoom, 1.6f, scalingFactor->getText().getFloatValue() / 100.0f);
    return ret;
}

void ParamDialog::setIsStandardTuning( bool b ) {
    is_standard_tuning_ = b;

    // Allow to see the actual tuning always.
/*
    // Enable to ''showTunButton'' only if the tuning
    // is not the standard one.
    if( showTunButton != nullptr )
        showTunButton->setEnabled( ! b );
*/
    if( resetTuningButton != nullptr )
        resetTuningButton->setEnabled( ! b );
}

// Force the internal component dialog to have keyboard focus. Ugly, but it is
// the only way I have found (including on the JUCE forum).
void ParamDialog::timerCallback() {
    stopTimer();
    grabKeyboardFocus();
}

static juce::StringArray dialogParamIds() {
    return {
        IDs::pitchRangeUp.name,    IDs::pitchRangeDown.name,  IDs::pitchStep.name,
        IDs::transposeTuningScale.name, IDs::glissando.name,  IDs::portamentoTm.name,
        IDs::modWheel.name,        IDs::modWheelPitch.name,   IDs::modWheelAmp.name,   IDs::modWheelEgBias.name,
        IDs::modFoot.name,         IDs::modFootPitch.name,    IDs::modFootAmp.name,    IDs::modFootEgBias.name,
        IDs::modBreath.name,       IDs::modBreathPitch.name,  IDs::modBreathAmp.name,  IDs::modBreathEgBias.name,
        IDs::modAftertouch.name,   IDs::modAftertouchPitch.name, IDs::modAftertouchAmp.name, IDs::modAftertouchEgBias.name,
        IDs::engineType.name
    };
}

void ParamDialog::refreshProfileList() {
    profileSelector->clear();
    profileSelector->addItem(AVDexedLanguageService::getInstance().text("selectProfile"), 1);

    juce::File profileDir = DexedAudioProcessor::dexedAppDir.getChildFile(AVDexedLibrary::profilesFolderName);
    if (profileDir.exists()) {
        juce::Array<juce::File> files;
        profileDir.findChildFiles(files, juce::File::findFiles, false, "*.xml");
        files.sort();
        for (int i = 0; i < files.size(); ++i)
            profileSelector->addItem(files[i].getFileNameWithoutExtension(), i + 2);
    }
    profileSelector->setSelectedItemIndex(0, juce::dontSendNotification);
}

void ParamDialog::AVAskProfileName() {
    auto& language = AVDexedLanguageService::getInstance();
    const auto safe = Component::SafePointer<ParamDialog>(this);
    // In the editor, as every other dialog: an AlertWindow is a desktop window
    // that could be dragged away, ignored the zoom and opened off-centre over
    // Settings. The validator keeps the prompt open on an unusable name.
    if (auto* editor = findParentComponentOfClass<DexedAudioProcessorEditor>()) {
        AVDexedPromptOverlay::Options options;
        options.title = language.text("saveProfileTitle");
        options.message = language.text("saveProfilePrompt");
        options.hasEditor = true;
        options.placeholder = language.text("profileName");
        options.confirmText = language.text("save");
        options.cancelText = language.text("cancel");
        options.validate = [](const String& name) { return AVProfileNameProblem(name); };
        editor->showAVPrompt(std::move(options), [safe](bool confirmed, const String& name) {
            if (confirmed && safe != nullptr) safe->saveProfileToFile(name);
        });
        return;
    }
    // No editor around this page: the AV alert window.
    auto* aw = new AVAlertWindow(language.text("saveProfileTitle"), language.text("saveProfilePrompt"), juce::MessageBoxIconType::NoIcon, this);
    aw->addTextEditor("name", "", language.text("profileName"));
    aw->addButton(language.text("save"), 1, KeyPress(KeyPress::returnKey));
    aw->addButton(language.text("cancel"), 0, KeyPress(KeyPress::escapeKey));
    aw->enterModalState(true, juce::ModalCallbackFunction::create([safe, aw](int result) {
        if (result == 1 && safe != nullptr) safe->saveProfileToFile(aw->getTextEditorContents("name"));
    }), true);
}

String ParamDialog::AVProfileNameProblem(const String& requested) {
    auto& language = AVDexedLanguageService::getInstance();
    const auto name = requested.trim();
    // A separator would put the file somewhere else.
    if (name.containsAnyOf("/\\:"))
        return language.text("profileNameInvalid");
    // `<name>.xml` must be a plain, visible file on every system: no reserved
    // or control characters, no leading dot (hidden, so never listed), no
    // trailing dot (Windows drops it), not a Windows device name.
    static const StringArray devices { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6",
                                       "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
                                       "LPT7", "LPT8", "LPT9" };
    bool control = false;
    for (auto c : name) control = control || c < 32 || c == 127;
    if (name.isEmpty() || control || name.containsAnyOf("*?\"<>|") || name.startsWithChar('.') || name.endsWithChar('.')
        || devices.contains(name.upToFirstOccurrenceOf(".", false, false).trim(), true))
        return language.text("profileNameUnusable");
    return {};
}

void ParamDialog::saveProfileToFile(const String& requested) {
    auto& language = AVDexedLanguageService::getInstance();
    // Errors here are in-editor notices (AVShowMessage finds the editor).
    const auto name = requested.trim();
    if (const auto problem = AVProfileNameProblem(name); problem.isNotEmpty()) {
        AVShowMessage(AlertWindow::WarningIcon, language.text("profileError"), problem, this);
        return;
    }
    const auto profileDir = DexedAudioProcessor::dexedAppDir.getChildFile(AVDexedLibrary::profilesFolderName);
    if (!profileDir.createDirectory()) {
        AVShowMessage(AlertWindow::WarningIcon, language.text("saveError"),
                      language.format("writeError", {{"path", profileDir.getFullPathName()}}), this);
        return;
    }
    ValueTree profileVt("DexedProfile");
    for (const auto& paramId : dialogParamIds()) {
        if (auto* param = apvts_.getParameter(paramId)) {
            ValueTree paramVt("param");
            paramVt.setProperty(IDs::id, paramId, nullptr);
            paramVt.setProperty(IDs::value, param->convertFrom0to1(param->getValue()), nullptr);
            profileVt.addChild(paramVt, -1, nullptr);
        }
    }
    ValueTree tuningVt("tuning");
    tuningVt.setProperty("scl", String(proc_.currentSCLData), nullptr);
    tuningVt.setProperty("kbm", String(proc_.currentKBMData), nullptr);
    profileVt.addChild(tuningVt, -1, nullptr);
    const auto destination = profileDir.getChildFile(name + ".xml");
    TemporaryFile temporary(destination);
    auto xml = profileVt.createXml();
    if (!xml->writeTo(temporary.getFile()) || !temporary.overwriteTargetFileWithTemporary()) {
        AVShowMessage(AlertWindow::WarningIcon, language.text("saveError"),
                      language.format("writeError", {{"path", destination.getFullPathName()}}), this);
        return;
    }
    refreshProfileList();
}

void ParamDialog::loadProfileFromFile(const juce::File& file) {
    auto xml = juce::XmlDocument::parse(file);
    auto profileVt = xml != nullptr ? juce::ValueTree::fromXml(*xml) : ValueTree();
    bool valid = profileVt.hasType("DexedProfile");
    const auto supported = dialogParamIds();
    StringArray seenIds;
    for (const auto& child : profileVt) {
        if (child.hasType("param")) {
            const auto id = child.getProperty(IDs::id).toString();
            // ValueTree XML attributes are strings, including numeric values.
            // Consume the complete number so corrupt text cannot silently turn
            // into zero while retaining compatibility with existing profiles.
            const auto valueText = child.getProperty(IDs::value).toString().trim();
            auto cursor = valueText.getCharPointer();
            const auto start = cursor;
            const auto value = static_cast<float>(CharacterFunctions::readDoubleValue(cursor));
            valid &= supported.contains(id) && !seenIds.contains(id) && std::isfinite(value)
                     && valueText.containsAnyOf("0123456789") && cursor != start && cursor.isEmpty();
            seenIds.add(id);
            if (auto* parameter = apvts_.getParameter(id))
                valid &= value >= parameter->getNormalisableRange().start
                         && value <= parameter->getNormalisableRange().end;
            else valid = false;
        } else if (child.hasType("tuning")) {
            const auto scl = child.getProperty("scl").toString();
            const auto kbm = child.getProperty("kbm").toString();
            valid &= scl.getNumBytesAsUTF8() <= MAX_SCL_KBM_FILE_SIZE && kbm.getNumBytesAsUTF8() <= MAX_SCL_KBM_FILE_SIZE;
            try {
                const auto scale = scl.isEmpty() ? Tunings::evenTemperament12NoteScale() : Tunings::parseSCLData(scl.toStdString());
                const auto mapping = kbm.isEmpty() ? Tunings::KeyboardMapping() : Tunings::parseKBMData(kbm.toStdString());
                const Tunings::Tuning validated(scale, mapping);
                ignoreUnused(validated);
            } catch (const std::exception&) { valid = false; }
        } else valid = false;
    }
    if (!valid) {
        auto& language = AVDexedLanguageService::getInstance();
        AVShowMessage(AlertWindow::WarningIcon, language.text("profileError"),
                      language.text("invalidProfile"), this);
        return;
    }
    for (int i = 0; i < profileVt.getNumChildren(); ++i) {
        auto child = profileVt.getChild(i);
        if (child.hasType("param")) {
            juce::String paramId = child.getProperty(IDs::id).toString();
            float value = static_cast<float>(child.getProperty(IDs::value));
            if (auto* param = apvts_.getParameter(paramId)) {
                param->beginChangeGesture();
                param->setValueNotifyingHost(param->convertTo0to1(value));
                param->endChangeGesture();
            }
        } else if (child.hasType("tuning")) {
            juce::String sclData = child.getProperty("scl").toString();
            juce::String kbmData = child.getProperty("kbm").toString();
            proc_.retuneToStandard();
            if (sclData.isNotEmpty())
                proc_.applySCLTuning(sclData.toStdString());
            if (kbmData.isNotEmpty())
                proc_.applyKBMMapping(kbmData.toStdString());
        }
    }
}

//[/MiscUserCode]
