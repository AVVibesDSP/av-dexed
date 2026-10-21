#include <map>


#include "OperatorEditor.h"
#include "PluginEditor.h"
#include "component/OperatorDisplay.h"
#include "util/ContextMenuAdapter.h"
#include "AVDexedTheme.h"
#include "AVDexedLanguage.h"

#ifndef M_LN10
#define M_LN10 2.30258509299404568402
#endif

class OperatorSwitch : public ToggleButton {
public :
    OperatorSwitch(juce::String name) : ToggleButton(name) {
        setSize(32, 32);
        setTooltip("Enable or disable this operator");
    }

    void paintButton(Graphics& g, bool isMouseOverButton, bool isButtonDown) override {
        // GOZO state pill: ocean letters mean "on"; hover only lifts the box.
        const auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        AVDexedTheme::fillBox(g, bounds, 9.0f, isMouseOverButton && !isButtonDown, 1.3f);
        if (AVDexedTheme::showsFocus(*this)) {
            g.setColour(AVDexedTheme::oceanHighlight);
            g.drawRoundedRectangle(bounds, 9.0f, 1.2f);
        }
        const auto text = AVDexedLanguageService::getInstance().text(getToggleState() ? "operatorOn" : "operatorOff");
        const auto font = AVDexedTheme::font(11.0f, "SemiBold");
        if (getToggleState()) AVDexedTheme::drawOceanText(g, text, font, bounds);
        else AVDexedTheme::drawGreyText(g, text, font, bounds, Justification::centred, 0.6f);
    }
};

//[/MiscUserDefs]


class OperatorContextMenu : public ContextMenuAdapter {
    int op;
public:
    OperatorContextMenu(DexedAudioProcessor &processor, int op) : ContextMenuAdapter(processor), op(op) {
    }

    void addSpecificItems(PopupMenu &menu, juce::Component *source) override {
        auto& language = AVDexedLanguageService::getInstance();
        Component::SafePointer<Component> safeSource(source);
        auto* target = &processor;
        const int index = op;
        menu.addItem(language.text("copyOperatorValues"), [safeSource, target, index] {
            if (safeSource) target->copyToClipboard(index);
        });
        menu.addItem(language.text("pasteEnvelopeValues"), DexedClipboard().isOperatorData(), false, [safeSource, target, index] {
            if (safeSource) target->pasteEnvFromClipboard(index);
        });
        menu.addItem(language.text("pasteOperatorValues"), DexedClipboard().isOperatorData(), false, [safeSource, target, index] {
            if (safeSource) target->pasteOpFromClipboard(index);
        });
        menu.addSeparator();
    }
};

//==============================================================================
OperatorEditor::OperatorEditor(DexedAudioProcessor& processor, int num)
    : processor(processor), internalOp(5 - num), AVOperatorIndex(num) {
    binder = std::make_unique<AudioComponentContainer>(*this, processor.parameters);
    opNum = String(num + 1);
    setFocusContainerType(FocusContainerType::focusContainer);
    const auto knob = [this](const String& id) {
        auto slider = std::make_unique<DXSlider>(id);
        slider->setSliderStyle(Slider::RotaryVerticalDrag);
        slider->setTextBoxStyle(Slider::TextBoxBelow, false, 52, 16);
        slider->setColour(Slider::textBoxOutlineColourId, Colours::transparentBlack);
        slider->setColour(Slider::textBoxBackgroundColourId, Colours::transparentBlack);
        slider->setWantsKeyboardFocus(true);
        binder->addAndAttach(std::move(slider));
    };
    knob(IDs::detune.op(num).name);
    knob(IDs::frequencyCoarse.op(num).name);
    knob(IDs::frequencyFine.op(num).name);
    knob(IDs::ampModeSens.op(num).name);
    knob(IDs::keyVelocity.op(num).name);
    knob(IDs::outputLevel.op(num).name);
    for (int step = 0; step < 4; ++step) {
        knob(IDs::egLevel.op(num).idx(step).name);
        knob(IDs::egRate.op(num).idx(step).name);
    }
    knob(IDs::lScaleDepth.op(num).name);
    knob(IDs::breakpoint.op(num).name);
    knob(IDs::rScaleDepth.op(num).name);
    knob(IDs::rateScaling.op(num).name);

    opMode = std::make_unique<ToggleButton>(IDs::mode.op(num).name);
    opMode->onStateChange = [this] { repaint(); };
    addAndMakeVisible(*opMode);
    binder->attach(opMode.get());
    opSwitch = std::make_unique<OperatorSwitch>(IDs::on.op(num).name);
    addAndMakeVisible(*opSwitch);
    binder->attach(opSwitch.get());

    auto frequency = std::make_unique<OperatorDisplay>(processor.parameters, num);
    AVFrequencyDisplay = frequency.get();
    binder->add(std::move(frequency));
    // Curve IDs follow the visible operator, like all its other parameters.
    for (bool left : {true, false}) {
        auto curve = std::make_unique<CurveSelector>(left);
        curve->setName(left ? IDs::lKeyScale.op(num).name : IDs::rKeyScale.op(num).name);
        binder->attach(curve.get());
        binder->add(std::move(curve));
    }
    envDisplay = std::make_unique<EnvDisplay>(processor.parameters, num);
    addAndMakeVisible(*envDisplay);
    vu = std::make_unique<VuMeter>();
    addAndMakeVisible(*vu);
    contextMenuAdapter = std::make_unique<OperatorContextMenu>(processor, internalOp);
    addMouseListener(contextMenuAdapter.get(), true);
    setWantsKeyboardFocus(true);
    applyLanguage();
}

OperatorEditor::~OperatorEditor() {
    binder->freeAttachments();
    envDisplay = nullptr;
    vu = nullptr;
    contextMenuAdapter = nullptr;
}

//==============================================================================
void OperatorEditor::paint(Graphics& g) {
    // GOZO Control/Mix panel: the 12% grey box, square, no outline.
    AVDexedTheme::fillBox(g, getLocalBounds().toFloat(), 0.0f);
    if (AVFlash > 0.0f) {
        g.setColour(AVDexedTheme::oceanHighlight.withAlpha(0.08f * AVFlash));
        g.fillRect(getLocalBounds());
        g.setColour(AVDexedTheme::oceanHighlight.withAlpha(AVFlash));
        g.drawRect(getLocalBounds().toFloat().reduced(1.0f), 2.0f);
    }
    auto& language = AVDexedLanguageService::getInstance();
    AVDexedTheme::drawGreyText(g, language.text("operator") + " " + opNum, AVDexedTheme::font(15.0f, "SemiBold"),
                               {12.0f, 4.0f, 130.0f, 26.0f}, Justification::centredLeft);
    for (const auto& label : AVLabels) {
        const auto font = AVDexedTheme::font(label.heading ? 12.0f : 11.0f, label.heading ? "SemiBold" : "Medium");
        g.setFont(font);
        const float inkHeight = jmin(float(label.bounds.getHeight()), font.getHeight());
        const float inkTop = float(label.bounds.getCentreY()) - inkHeight * 0.5f;
        AVDexedTheme::textFill(g, font, AVDexedTheme::textPrimary, AVDexedTheme::textSecondary, inkTop, inkTop + inkHeight);
        g.drawText(language.text(label.key), label.bounds, Justification::centred, true);
    }

}

void OperatorEditor::AVPlace(const String& id, Rectangle<int> bounds) {
    for (auto* child : getChildren()) {
        if (child->getName() == id) {
            child->setBounds(bounds);
            return;
        }
    }
}

void OperatorEditor::resized() {
    AVLabels.clear();
    const int w = getWidth();
    // Laid out on a 243 px reference height; taller panels spread the rows.
    const float k = jmax(1.0f, float(getHeight()) / 243.0f);
    const auto Y = [k](int v) { return roundToInt(float(v) * k); };
    const int envelopeWidth = roundToInt(w * 0.53f);
    const int cell = (w - 24) / 7;
    const int op = AVOperatorIndex;
    opSwitch->setBounds(w - 62, 6, 50, 22);
    AVFrequencyDisplay->setBounds(140, 5, jmax(40, w - 300), 24);
    vu->setBounds(w - 151, 13, 72, 8);
    const String ids[] { IDs::detune.op(op).name, IDs::frequencyCoarse.op(op).name,
        IDs::frequencyFine.op(op).name, {}, IDs::ampModeSens.op(op).name,
        IDs::keyVelocity.op(op).name, IDs::outputLevel.op(op).name };
    const String keys[] { "AV_DETUNE", "COARSE", "FINE", "FIXED", "AV_AMP_MOD", "AV_KEY_VEL", "LEVEL" };
    for (int i = 0; i < 7; ++i) {
        auto area = Rectangle<int>(12 + i * cell, Y(48), cell - 2, Y(48));
        AVLabels.push_back({keys[i], area.withY(Y(33)).withHeight(16)});
        if (i == 3) opMode->setBounds(area.withY(Y(56)).withHeight(24).reduced(7, 0));
        else AVPlace(ids[i], area);
    }
    AVLabels.push_back({"ENVELOPE_LEVEL", {12, Y(101), envelopeWidth - 20, 16}, true});
    AVLabels.push_back({"KEYBOARD_SCALING", {envelopeWidth + 4, Y(101), w - envelopeWidth - 16, 16}, true});
    envDisplay->setBounds(16, Y(120), envelopeWidth - 26, Y(18));
    const int egCell = (envelopeWidth - 42) / 4;
    for (int i = 0; i < 4; ++i) {
        const int x = 39 + i * egCell;
        AVLabels.push_back({String(i + 1), {x, Y(139), egCell, 13}});
        AVPlace(IDs::egLevel.op(op).idx(i).name, {x, Y(151), egCell - 1, Y(42)});
        AVPlace(IDs::egRate.op(op).idx(i).name, {x, Y(195), egCell - 1, Y(42)});
    }
    AVLabels.push_back({"L", {8, Y(160), 32, 18}});
    AVLabels.push_back({"R", {8, Y(204), 32, 18}});
    const int scaleCell = (w - envelopeWidth - 16) / 3;
    const String scaleIds[] {IDs::lScaleDepth.op(op).name, IDs::breakpoint.op(op).name, IDs::rScaleDepth.op(op).name};
    const String scaleKeys[] {"LEFT_DEPTH", "AV_BREAKPOINT", "RIGHT_DEPTH"};
    for (int i = 0; i < 3; ++i) {
        const int x = envelopeWidth + 4 + i * scaleCell;
        AVLabels.push_back({scaleKeys[i], {x, Y(120), scaleCell - 2, 16}});
        AVPlace(scaleIds[i], {x, Y(136), scaleCell - 2, Y(46)});
    }
    const int sx = envelopeWidth + 4;
    AVLabels.push_back({"AV_LEFT_CURVE", {sx, Y(186), scaleCell - 2, 15}});
    AVLabels.push_back({"AV_RATE_SCALE", {sx + scaleCell, Y(186), scaleCell - 2, 15}});
    AVLabels.push_back({"AV_RIGHT_CURVE", {sx + 2 * scaleCell, Y(186), scaleCell - 2, 15}});
    AVPlace(IDs::lKeyScale.op(op).name, {sx + 3, Y(206), scaleCell - 8, 26});
    AVPlace(IDs::rateScaling.op(op).name, {sx + scaleCell, Y(201), scaleCell - 2, Y(38)});
    AVPlace(IDs::rKeyScale.op(op).name, {sx + 2 * scaleCell + 3, Y(206), scaleCell - 8, 26});
}

//[MiscUserCode] You can add your own definitions of your custom methods or any other code here...

void OperatorEditor::updateGain(float v) {
    vu->setLevel(v);
}

void OperatorEditor::updateDisplay() {
}


void OperatorEditor::updateEnvPos(char pos) {
    // The stage changes a few times per note; the 50 Hz updates in between change nothing.
    if (envDisplay->vPos == pos) return;
    envDisplay->vPos = pos;
    envDisplay->repaint();
}

void OperatorEditor::mouseDown(const MouseEvent &event) {

}

void OperatorEditor::flashHighlight() {
    AVFlash = 1.0f;
    AVFlashStart = Time::getMillisecondCounterHiRes();
    AVFlashAnimation.start();
    repaint();
}

bool OperatorEditor::AVStepFlash(double now) {
    // Fades over about a second (what 60 steps of a 16 ms timer took).
    AVFlash = jmax(0.0f, 1.0f - float((now - AVFlashStart) / 960.0));
    repaint();
    return AVFlash > 0.0f;
}

void OperatorEditor::toggleOpSwitch() {
    opSwitch->setToggleState(!opSwitch->getToggleState(), sendNotification);
}

void OperatorEditor::applyLanguage() {
    auto& language = AVDexedLanguageService::getInstance();
    setTitle(language.text("operator") + " " + opNum);
    if (opSwitch != nullptr) {
        opSwitch->setTooltip(language.text("operatorToggleTooltip"));
        opSwitch->setTitle(language.text("operatorToggleTitle"));
    }
    const int op = AVOperatorIndex;
    std::map<String, String> captions {
        {IDs::detune.op(op).name, "DETUNE"}, {IDs::frequencyCoarse.op(op).name, "COARSE"},
        {IDs::frequencyFine.op(op).name, "FINE"}, {IDs::mode.op(op).name, "FREQUENCY_MODE"},
        {IDs::ampModeSens.op(op).name, "AMP_MOD"}, {IDs::keyVelocity.op(op).name, "KEY_VEL"},
        {IDs::outputLevel.op(op).name, "LEVEL"}, {IDs::lScaleDepth.op(op).name, "LEFT_DEPTH"},
        {IDs::rScaleDepth.op(op).name, "RIGHT_DEPTH"}, {IDs::breakpoint.op(op).name, "BREAKPOINT"},
        {IDs::rateScaling.op(op).name, "RATE_SCALE"}, {IDs::lKeyScale.op(op).name, "LEFT_CURVE"},
        {IDs::rKeyScale.op(op).name, "RIGHT_CURVE"}
    };
    for (auto* child : getChildren()) {
        String title;
        if (auto found = captions.find(child->getName()); found != captions.end()) title = language.text(found->second);
        for (int step = 0; step < 4; ++step) {
            if (child->getName() == IDs::egLevel.op(op).idx(step).name) title = language.text("ENVELOPE_LEVEL") + " " + String(step + 1);
            if (child->getName() == IDs::egRate.op(op).idx(step).name) title = language.text("RATE") + " " + String(step + 1);
        }
        if (title.isEmpty()) continue;
        title = language.text("operator") + " " + opNum + String::fromUTF8(" · ") + title;
        child->setTitle(title);
        if (auto* tooltip = dynamic_cast<SettableTooltipClient*>(child)) tooltip->setTooltip(title);
    }
    repaint();
}

//[/MiscUserCode]
