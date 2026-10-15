#include <map>

#include "GlobalEditor.h"
#include "DXLookNFeel.h"
#include "parameter/Model.h"
#include "ui/util/ContextMenuAdapter.h"
#include "../debugger/ValueTreeDebugger.h"
#include "AVDexedTheme.h"
#include "AVDexedLanguage.h"

/**
 * Ugly but useful midi monitor to know if you are really sending/receiving something from the DX7
 * If the midi is not configured this component wont show up
 */
#ifdef IMPLEMENT_MidiMonitor
class MidiMonitor : public Component {
    SysexComm *midi;
public:
    MidiMonitor(SysexComm *sysexComm) {
        midi = sysexComm;
        TRACE("WARNING! This functionality is a candidate for deprecation/obsolescence!");
    }

    void paint(Graphics &g) {
        if ( ! (midi->isInputActive() || midi->isOutputActive() ) )
            return;
        g.setColour(AVDexedTheme::textPrimary);

        if ( midi->isInputActive() ) {
            g.drawSingleLineText("DEXED IN", 24, 18);
            g.setColour(midi->inActivity ? AVDexedTheme::oceanHighlight : Colour(0xff34404a));
            g.fillEllipse(4.0f, 6.0f, 10.0f, 10.0f);
            midi->inActivity = false;
        }

        if ( midi->isOutputActive() ) {
            g.drawSingleLineText("DEXED OUT", 24, 36);
            g.setColour(midi->outActivity ? AVDexedTheme::oceanHighlight : Colour(0xff34404a));
            g.fillEllipse(4.0f, 24.0f, 10.0f, 10.0f);
            midi->outActivity = false;
        }
    }
};
#endif //IMPLEMENT_MidiMonitor

//[/MiscUserDefs]

class GlobalEditorContextMenu : public ContextMenuAdapter {
public:
    GlobalEditorContextMenu(DexedAudioProcessor &processor) : ContextMenuAdapter(processor) {
    }

    // For now we only support OP env, like dexed 1.0
    //
    // void addSpecificItems(PopupMenu &menu, juce::Component *source) override {
    //     menu.addItem("Copy Pitch Env Values", [this] {
    //     });
    //     menu.addItem("Paste Envelope Values", DexedClipboard().isOperatorData(), false, [this] {
    //     });
    //     menu.addSeparator();
    // }
};

//==============================================================================
GlobalEditor::GlobalEditor(DexedAudioProcessor &processor, juce::Component *parent) : processor(processor) {
    binder = std::make_unique<AudioComponentContainer>(*this, processor.parameters);
    SharedResourcePointer<DXLookNFeel> lookAndFeel;

    algoDisplay.reset(new AlgoDisplay(processor.parameters));
    addAndMakeVisible(algoDisplay.get());
    algoDisplay->setBounds(335, 30, 152, 91);

    auto algo = std::make_unique<DXSlider>(IDs::algorithm.name);
    algo->setExplicitFocusOrder(11);
    algo->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    algo->setTextBoxStyle(juce::Slider::NoTextBox, true, 80, 20);
    algo->setBounds(501, 22, 34, 34);
    binder->addAndAttach(std::move(algo));

    auto feedback = std::make_unique<DXSlider>(IDs::feedback.name);
    feedback->setExplicitFocusOrder(12);
    feedback->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    feedback->setTextBoxStyle(juce::Slider::NoTextBox, true, 80, 20);
    feedback->setBounds(501, 81, 34, 34);
    binder->addAndAttach(std::move(feedback));

    auto lfoSpeed = std::make_unique<DXSlider>(IDs::lfoRate.name);
    lfoSpeed->setExplicitFocusOrder(14);
    lfoSpeed->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    lfoSpeed->setTextBoxStyle(juce::Slider::NoTextBox, false, 80, 20);
    lfoSpeed->setBounds(564, 50, 34, 34);
    binder->addAndAttach(std::move(lfoSpeed));

    auto lfoAmDepth = std::make_unique<DXSlider>(IDs::lfoAmpDepth.name);
    lfoAmDepth->setExplicitFocusOrder(19);
    lfoAmDepth->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    lfoAmDepth->setTextBoxStyle(juce::Slider::NoTextBox, false, 80, 20);
    lfoAmDepth->setBounds(686, 50, 34, 34);
    binder->addAndAttach(std::move(lfoAmDepth));

    auto lfoPitchDepth = std::make_unique<DXSlider>(IDs::lfoPmDepth.name);
    lfoPitchDepth->setExplicitFocusOrder(18);
    lfoPitchDepth->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    lfoPitchDepth->setTextBoxStyle(juce::Slider::NoTextBox, false, 80, 20);
    lfoPitchDepth->setBounds(646, 50, 34, 34);
    binder->addAndAttach(std::move(lfoPitchDepth));

    auto lfoDelay = std::make_unique<DXSlider>(IDs::lfoDelay.name);
    lfoDelay->setExplicitFocusOrder(15);
    lfoDelay->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    lfoDelay->setTextBoxStyle(juce::Slider::NoTextBox, false, 80, 20);
    lfoDelay->setBounds(603, 50, 34, 34);
    binder->addAndAttach(std::move(lfoDelay));

    auto pitchRate1 = std::make_unique<DXSlider>(IDs::pitchEgRate.idx(0).name);
    pitchRate1->setExplicitFocusOrder(23);
    pitchRate1->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchRate1->setTextBoxStyle(juce::Slider::NoTextBox, false, 80, 20);
    pitchRate1->setBounds(739, 96, 34, 34);
    binder->addAndAttach(std::move(pitchRate1));

    auto pitchRate2 = std::make_unique<DXSlider>(IDs::pitchEgRate.idx(1).name);
    pitchRate2->setExplicitFocusOrder(24);
    pitchRate2->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchRate2->setTextBoxStyle(juce::Slider::NoTextBox, false, 80, 20);
    pitchRate2->setBounds(767, 96, 34, 34);
    binder->addAndAttach(std::move(pitchRate2));

    auto pitchRate3 = std::make_unique<DXSlider>(IDs::pitchEgRate.idx(2).name);
    pitchRate3->setExplicitFocusOrder(26);
    pitchRate3->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchRate3->setTextBoxStyle(juce::Slider::NoTextBox, false, 80, 20);
    pitchRate3->setBounds(795, 96, 35, 34);
    binder->addAndAttach(std::move(pitchRate3));

    auto pitchRate4 = std::make_unique<DXSlider>(IDs::pitchEgRate.idx(3).name);
    pitchRate4->setExplicitFocusOrder(28);
    pitchRate4->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchRate4->setTextBoxStyle(juce::Slider::NoTextBox, false, 80, 20);
    pitchRate4->setBounds(823, 96, 34, 34);
    binder->addAndAttach(std::move(pitchRate4));

    auto pitchLevel1 = std::make_unique<DXSlider>(IDs::pitchEgLevel.idx(0).name);
    pitchLevel1->setExplicitFocusOrder(21);
    pitchLevel1->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchLevel1->setTextBoxStyle(juce::Slider::NoTextBox, false, 80, 20);
    pitchLevel1->setBounds(739, 57, 34, 34);
    binder->addAndAttach(std::move(pitchLevel1));

    auto pitchLevel2 = std::make_unique<DXSlider>(IDs::pitchEgLevel.idx(1).name);
    pitchLevel2->setExplicitFocusOrder(23);
    pitchLevel2->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchLevel2->setTextBoxStyle(juce::Slider::NoTextBox, false, 80, 20);
    pitchLevel2->setBounds(767, 57, 34, 34);
    binder->addAndAttach(std::move(pitchLevel2));

    auto pitchLevel3 = std::make_unique<DXSlider>(IDs::pitchEgLevel.idx(2).name);
    pitchLevel3->setExplicitFocusOrder(25);
    pitchLevel3->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchLevel3->setTextBoxStyle(juce::Slider::NoTextBox, false, 80, 20);
    pitchLevel3->setBounds(795, 56, 34, 34);
    binder->addAndAttach(std::move(pitchLevel3));

    auto pitchLevel4 = std::make_unique<DXSlider>(IDs::pitchEgLevel.idx(3).name);
    pitchLevel4->setExplicitFocusOrder(27);
    pitchLevel4->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchLevel4->setTextBoxStyle(juce::Slider::NoTextBox, false, 80, 20);
    pitchLevel4->setBounds(823, 56, 34, 34);
    binder->addAndAttach(std::move(pitchLevel4));

    auto cutoff = std::make_unique<DXSlider>(IDs::cutoff.name);
    cutoff->setExplicitFocusOrder(6);
    cutoff->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    cutoff->setTextBoxStyle(juce::Slider::NoTextBox, true, 80, 20);
    cutoff->setBounds(234, 9, 34, 34);
    binder->addAndAttach(std::move(cutoff));

    auto reso = std::make_unique<DXSlider>(IDs::resonance.name);
    reso->setExplicitFocusOrder(7);
    reso->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    reso->setTextBoxStyle(juce::Slider::NoTextBox, true, 80, 20);
    reso->setBounds(278, 9, 34, 34);
    binder->addAndAttach(std::move(reso));

    auto transpose = std::make_unique<DXSlider>(IDs::transpose.name);
    transpose->setExplicitFocusOrder(9);
    transpose->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    transpose->setTextBoxStyle(juce::Slider::NoTextBox, true, 80, 20);
    transpose->setBounds(202, 60, 34, 34);
    binder->addAndAttach(std::move(transpose));

    auto pitchModSens = std::make_unique<DXSlider>(IDs::pitchModSens.name);
    pitchModSens->setExplicitFocusOrder(17);
    pitchModSens->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    pitchModSens->setTextBoxStyle(juce::Slider::NoTextBox, false, 80, 20);
    pitchModSens->setBounds(666, 5, 34, 34);
    binder->addAndAttach(std::move(pitchModSens));

    auto output = std::make_unique<DXSlider>(IDs::output.name);
    output->setExplicitFocusOrder(8);
    output->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    output->setTextBoxStyle(juce::Slider::NoTextBox, true, 80, 20);
    output->setBounds(157, 60, 34, 34);
    binder->addAndAttach(std::move(output));

    auto tune = std::make_unique<DXSlider>(IDs::masterTuneAdj.name);
    tune->setExplicitFocusOrder(5);
    tune->setSliderStyle(juce::Slider::RotaryVerticalDrag);
    tune->setTextBoxStyle(juce::Slider::NoTextBox, true, 80, 20);
    tune->setBounds(190, 9, 34, 34);
    binder->addAndAttach(std::move(tune));

    lfoType.reset(new WaveformSelector());
    addAndMakeVisible(lfoType.get());
    lfoType->setExplicitFocusOrder(13);
    lfoType->setName(IDs::lfoWaveform.name);
    lfoType->setBounds(583, 8, 36, 26);
    binder->attach(lfoType.get());

    // EVENT DRIVEN COMPONENTS

    monoMode = std::make_unique<juce::ToggleButton>(IDs::monoMode.name);
    monoMode->setExplicitFocusOrder(10);
    monoMode->setButtonText(juce::String());
    monoMode->setBounds(249, 65, 48, 26);
    monoMode->onStateChange = [this]() {
        repaint();
    };
    binder->attach(monoMode.get());
    addAndMakeVisible(monoMode.get());

    lfoSync = std::make_unique<juce::ToggleButton>(IDs::lfoKeySync.name);
    lfoSync->setExplicitFocusOrder(16);
    lfoSync->setButtonText(juce::String());
    lfoSync->setBounds(565, 96, 48, 26);
    lfoSync->onStateChange = [this]() {
        repaint();
    };
    binder->attach(lfoSync.get());
    addAndMakeVisible(lfoSync.get());

    oscSync = std::make_unique<juce::ToggleButton>(IDs::oscKeySync.name);
    oscSync->setExplicitFocusOrder(20);
    oscSync->setButtonText(juce::String());
    oscSync->setBounds(650, 96, 48, 26);
    oscSync->onStateChange = [this]() {
        repaint();
    };
    binder->attach(oscSync.get());
    addAndMakeVisible(oscSync.get());

    // EXTRA COMPONENTS

    pitchEnvDisplay.reset(new PitchEnvDisplay(processor.parameters));
    addAndMakeVisible(pitchEnvDisplay.get());
    pitchEnvDisplay->setBounds(751, 10, 93, 30);

    observer = std::make_unique<ParameterObserver>(parent, processor.parameters);
    addAndMakeVisible(observer.get());
    observer->setBounds(6, 87, 140, 13);

    vuOutput.reset(new VuMeterOutput());
    addAndMakeVisible(vuOutput.get());
    vuOutput->setName("vuOutput");
    vuOutput->setBounds(6, 103, 140, 8);

    initButton.reset(new juce::TextButton("initButton"));
    addAndMakeVisible(initButton.get());
    initButton->setExplicitFocusOrder(3);
    initButton->setButtonText(TRANS("INIT"));
    initButton->setBounds(100, 111, 50, 30);

    parmButton.reset(new juce::TextButton("parmButton"));
    addAndMakeVisible(parmButton.get());
    parmButton->setExplicitFocusOrder(2);
    parmButton->setButtonText(TRANS("PARM"));
    parmButton->setBounds(52, 111, 50, 30);

    cartButton.reset(new juce::TextButton("cartButton"));
    addAndMakeVisible(cartButton.get());
    cartButton->setExplicitFocusOrder(1);
    cartButton->setButtonText(TRANS("CART"));
    cartButton->setBounds(3, 111, 50, 30);

    storeButton.reset(new juce::TextButton("storeButton"));
    addAndMakeVisible(storeButton.get());
    storeButton->setExplicitFocusOrder(4);
    storeButton->setButtonText(TRANS("STORE"));
    storeButton->setBounds(270, 109, 50, 30);

    programSelector.reset(new ProgramSelector());
    addAndMakeVisible(programSelector.get());
    programSelector->setBounds(153, 115, 112, 18);

    aboutButton.reset(new juce::TextButton("DEXED"));
    addAndMakeVisible(aboutButton.get());
    aboutButton->setButtonText("DEXED");
    aboutButton->setTitle("About AV Dexed");
    aboutButton->setColour(TextButton::textColourOffId, AVDexedTheme::textPrimary);
    aboutButton->setColour(TextButton::textColourOnId, AVDexedTheme::textPrimary);
    aboutButton->onClick = [this]() {
        juce::ModifierKeys modifiers = juce::ModifierKeys::getCurrentModifiers();
        if( modifiers.isAltDown()  || modifiers.isShiftDown() ) {
            ValueTreeDebugger *vtd = new ValueTreeDebugger(this->processor.parameters.rootVt);
            debugger.reset(vtd);
            return;
        }
        // About opens inside the editor (Settings, on its About page).
        if (onAboutRequested) onAboutRequested();
    };
    aboutButton->setBounds(8, 11, 135, 46);

    for (auto* child : getChildren()) {
        if (auto* slider = dynamic_cast<DXSlider*>(child)) {
            slider->setTextBoxStyle(Slider::TextBoxBelow, false, 60, 16);
            slider->setColour(Slider::textBoxOutlineColourId, Colours::transparentBlack);
            slider->setColour(Slider::textBoxBackgroundColourId, Colours::transparentBlack);
        }
    }
    setSize(1392, 184);

    programs = programSelector.get();
    setTitle("Global Parameters");
    setFocusContainerType(FocusContainerType::focusContainer);
    setWantsKeyboardFocus(true);

#ifdef IMPLEMENT_MidiMonitor
    midiMonitor = std::make_unique<MidiMonitor>(&(processor->sysexComm));
    addAndMakeVisible(*midiMonitor);
    midiMonitor->setBounds(110, 10, 80, 45);    //midiMonitor->setBounds(155, 21, 80, 45);
#endif //IMPLEMENT_MidiMonitor

    contextMenuAdapter = std::make_unique<GlobalEditorContextMenu>(processor);
    addMouseListener(contextMenuAdapter.get(), true);

    //[/Constructor]
}

GlobalEditor::~GlobalEditor() {
    binder->freeAttachments();
    oscSync = nullptr;
    pitchEnvDisplay = nullptr;
    algoDisplay = nullptr;
    vuOutput = nullptr;
    initButton = nullptr;
    parmButton = nullptr;
    cartButton = nullptr;
    storeButton = nullptr;
    monoMode = nullptr;
    programSelector = nullptr;
    aboutButton = nullptr;
}

//==============================================================================
void GlobalEditor::paint(Graphics& g) {
    for (const auto& area : AVPanels) AVDexedTheme::fillBox(g, area, 0.0f);
    auto& language = AVDexedLanguageService::getInstance();
    for (const auto& label : AVLabels) {
        const auto font = AVDexedTheme::font(label.heading ? 12.0f : 11.0f, label.heading ? "SemiBold" : "Medium");
        g.setFont(font);
        const float inkHeight = jmin(float(label.bounds.getHeight()), font.getHeight());
        const float inkTop = float(label.bounds.getCentreY()) - inkHeight * 0.5f;
        AVDexedTheme::textFill(g, font, AVDexedTheme::textPrimary, AVDexedTheme::textSecondary, inkTop, inkTop + inkHeight);
        g.drawText(language.text(label.key), label.bounds, Justification::centred, true);
    }
}

void GlobalEditor::AVPlace(const String& id, Rectangle<int> bounds) {
    for (auto* child : getChildren()) if (child->getName() == id) {
        child->setBounds(bounds);
        return;
    }
}

void GlobalEditor::resized() {
    AVLabels.clear();
    AVPanels.clear();
    const int gap = 10;
    const int available = getWidth() - gap * 4;
    // The algorithm panel gets more room so its operator grid reads clearly.
    const int library = roundToInt(available * 0.18f);
    const int synthesis = roundToInt(available * 0.19f);
    const int algorithm = roundToInt(available * 0.23f);
    const int lfo = roundToInt(available * 0.22f);
    const int pitch = available - library - synthesis - algorithm - lfo;
    const int sx = library + gap;
    const int ax = sx + synthesis + gap;
    const int lx = ax + algorithm + gap;
    const int px = lx + lfo + gap;
    for (auto r : {Rectangle<int>(0, 0, library, getHeight()), {sx, 0, synthesis, getHeight()},
                    {ax, 0, algorithm, getHeight()}, {lx, 0, lfo, getHeight()}, {px, 0, pitch, getHeight()}})
        AVPanels.push_back(r.toFloat().reduced(0.5f));
    aboutButton->setBounds(12, 9, 108, 27);
    aboutButton->setButtonText("AV Dexed");
    observer->setBounds(128, 9, library - 140, 27);
    programSelector->setBounds(12, 43, library - 24, 32);
    vuOutput->setBounds(12, 84, library - 24, 7);
    const int bw = (library - 30) / 2;
    cartButton->setBounds(12, 103, bw, 30);
    parmButton->setBounds(18 + bw, 103, bw, 30);
    initButton->setBounds(12, 140, bw, 30);
    storeButton->setBounds(18 + bw, 140, bw, 30);

    const int sc = (synthesis - 24) / 3;
    const String synthIds[] {IDs::masterTuneAdj.name, IDs::cutoff.name, IDs::resonance.name,
                             IDs::output.name, IDs::transpose.name};
    const String synthKeys[] {"AV_MASTER_TUNE", "CUTOFF", "RESONANCE", "OUTPUT", "TRANSPOSE"};
    for (int i = 0; i < 5; ++i) {
        int x = sx + 12 + (i % 3) * sc;
        int y = i < 3 ? 10 : 91;
        AVLabels.push_back({synthKeys[i], {x, y, sc - 2, 22}});
        AVPlace(synthIds[i], {x, y + 23, sc - 2, 56});
    }
    AVLabels.push_back({"MONO", {sx + 12 + 2 * sc, 95, sc - 2, 24}});
    monoMode->setBounds(sx + 18 + 2 * sc, 128, sc - 14, 27);
    AVLabels.push_back({"ALGORITHM", {ax + 10, 8, algorithm - 20, 22}, true});
    algoDisplay->setBounds(ax + 10, 34, algorithm - 88, 138);
    AVPlace(IDs::algorithm.name, {ax + algorithm - 75, 34, 62, 55});
    AVLabels.push_back({"AV_FEEDBACK", {ax + algorithm - 80, 99, 73, 20}});
    AVPlace(IDs::feedback.name, {ax + algorithm - 75, 119, 62, 55});

    AVLabels.push_back({"LFO_WAVE", {lx + 10, 7, 128, 20}, true});
    lfoType->setBounds(lx + 15, 29, 114, 28);
    AVLabels.push_back({"PM_SENS", {lx + lfo - 130, 7, 115, 20}});
    AVPlace(IDs::pitchModSens.name, {lx + lfo - 100, 27, 70, 44});
    const int lc = (lfo - 24) / 4;
    const String lfoIds[] {IDs::lfoRate.name, IDs::lfoDelay.name, IDs::lfoPmDepth.name, IDs::lfoAmpDepth.name};
    const String lfoKeys[] {"SPEED", "DELAY", "PMD", "AMD"};
    for (int i = 0; i < 4; ++i) {
        int x = lx + 12 + i * lc;
        AVLabels.push_back({lfoKeys[i], {x, 70, lc - 2, 20}});
        AVPlace(lfoIds[i], {x, 91, lc - 2, 52});
    }
    lfoSync->setBounds(lx + 15, 149, 43, 24);
    oscSync->setBounds(lx + lfo / 2 + 4, 149, 43, 24);
    AVLabels.push_back({"KEY_SYNC", {lx + 61, 149, lfo / 2 - 65, 24}});
    AVLabels.push_back({"OSC_SYNC", {lx + lfo / 2 + 50, 149, lfo / 2 - 60, 24}});

    AVLabels.push_back({"PITCH_ENVELOPE", {px + 10, 6, pitch - 20, 22}, true});
    pitchEnvDisplay->setBounds(px + 15, 32, pitch - 30, 22);
    const int pc = (pitch - 24) / 4;
    for (int i = 0; i < 4; ++i) {
        int x = px + 12 + i * pc;
        AVLabels.push_back({"L" + String(i + 1), {x, 57, pc - 1, 17}});
        AVPlace(IDs::pitchEgLevel.idx(i).name, {x, 73, pc - 1, 43});
        AVLabels.push_back({"R" + String(i + 1), {x, 117, pc - 1, 17}});
        AVPlace(IDs::pitchEgRate.idx(i).name, {x, 133, pc - 1, 43});
    }
#ifdef IMPLEMENT_MidiMonitor
    midiMonitor->setBounds(128, 9, library - 140, 27);
#endif
}

void GlobalEditor::updateDisplay() {
    repaint();
}

void GlobalEditor::applyLanguage() {
    auto& language = AVDexedLanguageService::getInstance();
    if (initButton) initButton->setButtonText(language.text("classicInit"));
    if (parmButton) parmButton->setButtonText(language.text("classicSettings"));
    if (cartButton) cartButton->setButtonText(language.text("classicBank"));
    if (storeButton) storeButton->setButtonText(language.text("classicSave"));
    if (aboutButton) aboutButton->setTitle(language.text("aboutDexed"));
    setTitle(language.text("classicGlobalTitle"));
    std::map<String, String> captions {
        {IDs::masterTuneAdj.name, "MASTER_TUNE"}, {IDs::cutoff.name, "CUTOFF"}, {IDs::resonance.name, "RESONANCE"},
        {IDs::output.name, "OUTPUT"}, {IDs::transpose.name, "TRANSPOSE"}, {IDs::monoMode.name, "MONO"},
        {IDs::algorithm.name, "ALGORITHM"}, {IDs::feedback.name, "FEEDBACK"}, {IDs::lfoWaveform.name, "LFO_WAVE"},
        {IDs::lfoRate.name, "SPEED"}, {IDs::lfoDelay.name, "DELAY"}, {IDs::lfoPmDepth.name, "PMD"},
        {IDs::lfoAmpDepth.name, "AMD"}, {IDs::pitchModSens.name, "PM_SENS"}, {IDs::lfoKeySync.name, "KEY_SYNC"},
        {IDs::oscKeySync.name, "OSC_SYNC"}
    };
    for (auto* child : getChildren()) {
        String title;
        if (auto found = captions.find(child->getName()); found != captions.end()) title = language.text(found->second);
        for (int step = 0; step < 4; ++step) {
            if (child->getName() == IDs::pitchEgLevel.idx(step).name) title = language.text("PITCH_ENVELOPE") + " L" + String(step + 1);
            if (child->getName() == IDs::pitchEgRate.idx(step).name) title = language.text("PITCH_ENVELOPE") + " R" + String(step + 1);
        }
        if (title.isEmpty()) continue;
        child->setTitle(title);
        if (auto* tooltip = dynamic_cast<SettableTooltipClient*>(child)) tooltip->setTooltip(title);
    }
    repaint();
}

void GlobalEditor::updatePitchPos(int pos) {
    pitchEnvDisplay->vPos = pos;
    pitchEnvDisplay->repaint();
}

void GlobalEditor::updateVu(float f) {
    vuOutput->v = f;
    vuOutput->repaint();
#ifdef IMPLEMENT_MidiMonitor
    midiMonitor->repaint();
#endif //IMPLEMENT_MidiMonitor
}

//[/MiscUserCode]


//==============================================================================
#if 0
/*  -- Projucer information section --

    This is where the Projucer stores the metadata that describe this GUI layout, so
    make changes in here at your peril!

BEGIN_JUCER_METADATA

<JUCER_COMPONENT documentType="Component" className="GlobalEditor" componentName=""
                 parentClasses="public Component" constructorParams="" variableInitialisers=""
                 snapPixels="8" snapActive="0" snapShown="1" overlayOpacity="0.330"
                 fixedSize="1" initialWidth="864" initialHeight="144">
  <BACKGROUND backgroundColour="ffffff"/>
  <SLIDER name="lfoSpeed" id="b10eaf327ab3bff5" memberName="lfoSpeed" virtualName=""
          explicitFocusOrder="14" pos="564 50 34 34" min="0.0" max="99.0"
          int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox" textBoxEditable="1"
          textBoxWidth="80" textBoxHeight="20" skewFactor="1.0" needsCallback="1"/>
  <SLIDER name="lfoAmDepth" id="3e95a6206fa4a891" memberName="lfoAmDepth"
          virtualName="" explicitFocusOrder="19" pos="686 50 34 34" min="0.0"
          max="99.0" int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox"
          textBoxEditable="1" textBoxWidth="80" textBoxHeight="20" skewFactor="1.0"
          needsCallback="1"/>
  <SLIDER name="lfoPitchDepth" id="6ead769ca786c813" memberName="lfoPitchDepth"
          virtualName="" explicitFocusOrder="18" pos="646 50 34 34" min="0.0"
          max="99.0" int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox"
          textBoxEditable="1" textBoxWidth="80" textBoxHeight="20" skewFactor="1.0"
          needsCallback="1"/>
  <SLIDER name="lfoDelay" id="1fce68dc81619ef5" memberName="lfoDelay" virtualName=""
          explicitFocusOrder="15" pos="603 50 34 34" min="0.0" max="99.0"
          int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox" textBoxEditable="1"
          textBoxWidth="80" textBoxHeight="20" skewFactor="1.0" needsCallback="1"/>
  <SLIDER name="cutoff" id="40531f16bb0bd225" memberName="cutoff" virtualName=""
          explicitFocusOrder="6" pos="234 9 34 34" min="0.0" max="1.0"
          int="0.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox" textBoxEditable="0"
          textBoxWidth="80" textBoxHeight="20" skewFactor="1.0" needsCallback="1"/>
  <SLIDER name="reso" id="c8c13464e81a8d83" memberName="reso" virtualName=""
          explicitFocusOrder="7" pos="278 9 34 34" min="0.0" max="1.0"
          int="0.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox" textBoxEditable="0"
          textBoxWidth="80" textBoxHeight="20" skewFactor="1.0" needsCallback="1"/>
  <SLIDER name="pitchRate2" id="73f386b3c91d3de4" memberName="pitchRate2"
          virtualName="" explicitFocusOrder="24" pos="767 96 34 34" min="0.0"
          max="99.0" int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox"
          textBoxEditable="1" textBoxWidth="80" textBoxHeight="20" skewFactor="1.0"
          needsCallback="1"/>
  <SLIDER name="pitchRate3" id="187c25b24413fccf" memberName="pitchRate3"
          virtualName="" explicitFocusOrder="26" pos="795 96 35 34" min="0.0"
          max="99.0" int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox"
          textBoxEditable="1" textBoxWidth="80" textBoxHeight="20" skewFactor="1.0"
          needsCallback="1"/>
  <SLIDER name="pitchRate4" id="1aeb2a8fbbcbeaab" memberName="pitchRate4"
          virtualName="" explicitFocusOrder="28" pos="823 96 34 34" min="0.0"
          max="99.0" int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox"
          textBoxEditable="1" textBoxWidth="80" textBoxHeight="20" skewFactor="1.0"
          needsCallback="1"/>
  <SLIDER name="pitchRate1" id="419d613b3fb9604e" memberName="pitchRate1"
          virtualName="" explicitFocusOrder="22" pos="739 96 34 34" min="0.0"
          max="99.0" int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox"
          textBoxEditable="1" textBoxWidth="80" textBoxHeight="20" skewFactor="1.0"
          needsCallback="1"/>
  <SLIDER name="pitchLevel2" id="3a355ad2221887d9" memberName="pitchLevel2"
          virtualName="" explicitFocusOrder="23" pos="767 57 34 34" min="0.0"
          max="99.0" int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox"
          textBoxEditable="1" textBoxWidth="80" textBoxHeight="20" skewFactor="1.0"
          needsCallback="1"/>
  <SLIDER name="pitchLevel3" id="5c5d782e8a7f3ad7" memberName="pitchLevel3"
          virtualName="" explicitFocusOrder="25" pos="795 56 34 34" min="0.0"
          max="99.0" int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox"
          textBoxEditable="1" textBoxWidth="80" textBoxHeight="20" skewFactor="1.0"
          needsCallback="1"/>
  <SLIDER name="pitchLevel4" id="eb900b141fbad8ff" memberName="pitchLevel4"
          virtualName="" explicitFocusOrder="27" pos="823 56 34 34" min="0.0"
          max="99.0" int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox"
          textBoxEditable="1" textBoxWidth="80" textBoxHeight="20" skewFactor="1.0"
          needsCallback="1"/>
  <SLIDER name="pitchLevel1" id="344cbe26ec9fa128" memberName="pitchLevel1"
          virtualName="" explicitFocusOrder="21" pos="739 57 34 34" min="0.0"
          max="99.0" int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox"
          textBoxEditable="1" textBoxWidth="80" textBoxHeight="20" skewFactor="1.0"
          needsCallback="1"/>
  <SLIDER name="transpose" id="7d1266b1c1534947" memberName="transpose"
          virtualName="" explicitFocusOrder="9" pos="202 60 34 34" min="0.0"
          max="48.0" int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox"
          textBoxEditable="0" textBoxWidth="80" textBoxHeight="20" skewFactor="1.0"
          needsCallback="1"/>
  <TOGGLEBUTTON name="oscSync" id="8f3fe641537cd00" memberName="oscSync" virtualName=""
                explicitFocusOrder="20" pos="650 96 48 26" buttonText="" connectedEdges="0"
                needsCallback="1" radioGroupId="0" state="0"/>
  <SLIDER name="pitchModSens" id="904f73df85a9f886" memberName="pitchModSens"
          virtualName="" explicitFocusOrder="17" pos="666 5 34 34" min="0.0"
          max="7.0" int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox"
          textBoxEditable="1" textBoxWidth="80" textBoxHeight="20" skewFactor="1.0"
          needsCallback="1"/>
  <TOGGLEBUTTON name="lfoSync" id="ff92bb0a5a4f7187" memberName="lfoSync" virtualName=""
                explicitFocusOrder="16" pos="565 96 48 26" buttonText="" connectedEdges="0"
                needsCallback="1" radioGroupId="0" state="0"/>
  <GENERICCOMPONENT name="pitchEnvDisplay" id="9ddaae8ef924a038" memberName="pitchEnvDisplay"
                    virtualName="" explicitFocusOrder="0" pos="751 10 93 30" class="PitchEnvDisplay"
                    params=""/>
  <GENERICCOMPONENT name="algoDisplay" id="b26fb9e3b5f0bc37" memberName="algoDisplay"
                    virtualName="" explicitFocusOrder="0" pos="335 30 152 91" class="AlgoDisplay"
                    params=""/>
  <SLIDER name="feedback" id="4fac1940c29ab8c" memberName="feedback" virtualName=""
          explicitFocusOrder="12" pos="501 81 34 34" min="0.0" max="7.0"
          int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox" textBoxEditable="1"
          textBoxWidth="80" textBoxHeight="20" skewFactor="1.0" needsCallback="1"/>
  <SLIDER name="algo" id="8a226ddf9bbff752" memberName="algo" virtualName=""
          explicitFocusOrder="11" pos="501 22 34 34" min="1.0" max="32.0"
          int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox" textBoxEditable="0"
          textBoxWidth="80" textBoxHeight="20" skewFactor="1.0" needsCallback="1"/>
  <GENERICCOMPONENT name="lcdDisplay" id="30c7bb8f114cbbe3" memberName="lcdDisplay"
                    virtualName="" explicitFocusOrder="0" pos="6 87 140 13" class="LcdDisplay"
                    params=""/>
  <SLIDER name="output" id="7697fdd54fd1593e" memberName="output" virtualName=""
          explicitFocusOrder="8" pos="157 60 34 34" min="0.0" max="1.0"
          int="0.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox" textBoxEditable="0"
          textBoxWidth="80" textBoxHeight="20" skewFactor="1.0" needsCallback="1"/>
  <GENERICCOMPONENT name="vuOutput" id="dac75af912267f51" memberName="vuOutput" virtualName=""
                    explicitFocusOrder="0" pos="6 103 140 8" class="VuMeterOutput"
                    params=""/>
  <TEXTBUTTON name="initButton" id="92b278163c42e21d" memberName="initButton"
              virtualName="" explicitFocusOrder="3" pos="100 111 50 30" buttonText="INIT"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="parmButton" id="d5cd6260b34be817" memberName="parmButton"
              virtualName="" explicitFocusOrder="2" pos="52 111 50 30" buttonText="PARM"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="cartButton" id="465a101c3a0e744" memberName="cartButton"
              virtualName="" explicitFocusOrder="1" pos="3 111 50 30" buttonText="CART"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="storeButton" id="a94bec26832eb58b" memberName="storeButton"
              virtualName="" explicitFocusOrder="4" pos="270 109 50 30" buttonText="STORE"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <TOGGLEBUTTON name="monoMode" id="e683d8cbe286367b" memberName="monoMode" virtualName=""
                explicitFocusOrder="10" pos="249 65 48 26" buttonText="" connectedEdges="0"
                needsCallback="1" radioGroupId="0" state="0"/>
  <GENERICCOMPONENT name="lfoType" id="62f61dda9f76c8f" memberName="lfoType" virtualName=""
                    explicitFocusOrder="13" pos="583 8 36 26" class="ComboBoxImage"
                    params=""/>
  <GENERICCOMPONENT name="programSelector" id="990bbcccae72dbe6" memberName="programSelector"
                    virtualName="" explicitFocusOrder="0" pos="153 115 112 18" class="ProgramSelector"
                    params=""/>
  <IMAGEBUTTON name="aboutButton" id="d195a60b29440aa1" memberName="aboutButton"
               virtualName="" explicitFocusOrder="0" pos="8 11 135 46" buttonText=""
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="0"
               resourceNormal="" opacityNormal="1.0" colourNormal="0" resourceOver=""
               opacityOver="1.0" colourOver="0" resourceDown="" opacityDown="1.0"
               colourDown="0"/>
  <SLIDER name="tune" id="d22c34aa3363a28a" memberName="tune" virtualName=""
          explicitFocusOrder="5" pos="190 9 34 34" min="0.0" max="1.0"
          int="0.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox" textBoxEditable="0"
          textBoxWidth="80" textBoxHeight="20" skewFactor="1.0" needsCallback="1"/>
</JUCER_COMPONENT>

END_JUCER_METADATA
*/
#endif


//[EndFile] You can add extra defines here...
//[/EndFile]

