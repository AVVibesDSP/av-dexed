// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedSettingsPanel.h"
#include "../AVDexedTheme.h"
#include "../AVDexedStandaloneAccess.h"
#include "../AVDexedSettingsStyle.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <cmath>
#include <BinaryData.h>
#include "../../PluginProcessor.h"
#include "../../parameter/Model.h"
#include "../../midi/MidiCCHandler.h"

using namespace juce;

class AVDexedSettingsPanel::AVTabButton final : public Button {
public:
    explicit AVTabButton(int index) : Button(String(index)) { setWantsKeyboardFocus(true); }
    void paintButton(Graphics& g, bool hover, bool) override {
        // No press offset: the text stays still when clicked, as in GOZO.
        const auto bounds = getLocalBounds().toFloat().reduced(1.0f);
        if (getToggleState() || hover) {
            // Reference sidebar: transparent at the outer edge, illuminating
            // inward. AV's blue family replaces the source amber.
            ColourGradient highlight(AVDexedTheme::oceanTop.withAlpha(0.0f), bounds.getX(), bounds.getCentreY(),
                                     AVDexedTheme::oceanTop, bounds.getRight(), bounds.getCentreY(), false);
            highlight.multiplyOpacity(0.30f * (getToggleState() ? 1.0f : 0.55f));
            g.setGradientFill(highlight);
            g.fillRoundedRectangle(bounds, 9.0f);
        }
        if (AVDexedTheme::showsFocus(*this)) {
            g.setColour(AVDexedTheme::oceanHighlight);
            g.drawRoundedRectangle(bounds, 9.0f, 1.4f);
        }
        // GOZO: Regular 30, 27 px in from the highlight's edge, 12 px down.
        const auto label = Rectangle<float>(27.0f, 12.0f, float(getWidth()) - 33.0f, 30.0f);
        AVDexedTheme::drawGreyText(g, getButtonText(), AVDexedTheme::font(30.0f, "Regular"), label, Justification::centredLeft);
    }
};

namespace {
// GOZO settings vocabulary: section heading over a soft hairline, toggle rows
// with a ring and dot, and pills (the 12% box with ocean letters when "on").
/** GOZO's scroll thumb: a 6 px ocean capsule in a 12 px strip on the right,
    shown only when the page overflows. The wheel scrolls; the thumb drags. */
class AVScrollThumb final : public Component, private ComponentListener {
public:
    explicit AVScrollThumb(Viewport& viewport) : AVViewport(viewport) {
        viewport.setScrollBarsShown(false, false, true, false);
        if (auto* content = viewport.getViewedComponent()) content->addComponentListener(this);
        setMouseCursor(MouseCursor::UpDownResizeCursor);
    }
    ~AVScrollThumb() override {
        if (auto* content = AVViewport.getViewedComponent()) content->removeComponentListener(this);
    }
    void update() {
        auto* content = AVViewport.getViewedComponent();
        setVisible(content != nullptr && content->getHeight() > AVViewport.getHeight());
        repaint();
    }
    void paint(Graphics& g) override {
        const auto thumb = AVThumb();
        if (thumb.isEmpty()) return;
        g.setGradientFill(ColourGradient(AVDexedTheme::oceanHighlight, 0.0f, thumb.getY(), AVDexedTheme::oceanTop, 0.0f, thumb.getBottom(), false));
        g.fillRoundedRectangle(thumb, 3.0f);
    }
    void mouseDown(const MouseEvent&) override { AVStartY = AVViewport.getViewPositionY(); }
    void mouseDrag(const MouseEvent& event) override {
        auto* content = AVViewport.getViewedComponent();
        if (content == nullptr) return;
        const float track = float(getHeight() - 4) - AVThumb().getHeight();
        const float range = float(content->getHeight() - AVViewport.getHeight());
        if (track <= 0.0f || range <= 0.0f) return;
        AVViewport.setViewPosition(0, AVStartY + roundToInt(float(event.getDistanceFromDragStartY()) * range / track));
        repaint();
    }
    void mouseWheelMove(const MouseEvent& event, const MouseWheelDetails& wheel) override {
        AVViewport.mouseWheelMove(event.getEventRelativeTo(&AVViewport), wheel);
        repaint();
    }
private:
    Viewport& AVViewport;
    int AVStartY = 0;
    Rectangle<float> AVThumb() const {
        auto* content = AVViewport.getViewedComponent();
        if (content == nullptr || content->getHeight() <= AVViewport.getHeight()) return {};
        const float track = float(getHeight() - 4);
        const float height = jmax(40.0f, track * float(AVViewport.getHeight()) / float(content->getHeight()));
        const float range = float(content->getHeight() - AVViewport.getHeight());
        const float y = 2.0f + (track - height) * float(AVViewport.getViewPositionY()) / jmax(1.0f, range);
        return { float(getWidth() - 9), y, 6.0f, height };
    }
    void componentMovedOrResized(Component&, bool, bool) override { update(); }
};

/** Paints through a callback: lets a scrolling page draw its headings. */
struct AVPaintedContent final : public Component {
    std::function<void(Graphics&)> painter;
    void paint(Graphics& g) override { if (painter) painter(g); }
};

/** One MIDI input: ring + dot when enabled; the whole row toggles. */
class AVInputToggleRow final : public Component {
public:
    String name;
    bool on = false;
    std::function<void()> onToggle;
    AVInputToggleRow() { setMouseCursor(MouseCursor::PointingHandCursor); setRepaintsOnMouseActivity(true); }
    void paint(Graphics& g) override {
        const float cy = float(getHeight()) * 0.5f;
        const auto ring = Rectangle<float>(2.0f, cy - 8.0f, 16.0f, 16.0f);
        g.setColour(AVDexedTheme::oceanHighlight.withAlpha(on ? 1.0f : 0.45f));
        g.drawEllipse(ring.reduced(0.75f), 1.5f);
        if (on) {
            const auto dot = ring.reduced(4.5f);
            g.setGradientFill(ColourGradient(AVDexedTheme::oceanHighlight, 0.0f, dot.getY(), AVDexedTheme::oceanTop, 0.0f, dot.getBottom(), false));
            g.fillEllipse(dot);
        }
        const auto text = getLocalBounds().toFloat().withTrimmedLeft(28.0f);
        if (isMouseOver()) AVDexedTheme::drawOceanText(g, name, AVDexedTheme::font(15.0f), text, Justification::centredLeft);
        else AVDexedTheme::drawGreyText(g, name, AVDexedTheme::font(15.0f), text, Justification::centredLeft);
    }
    void mouseUp(const MouseEvent& event) override {
        if (!event.mouseWasDraggedSinceMouseDown() && getLocalBounds().contains(event.getPosition()) && onToggle) onToggle();
    }
};

/** "CC n" pill: drag up/down to pick another CC (6 px per step, CC 64 —
    sustain — skipped), as in GOZO. */
class AVCCPill final : public Component, public SettableTooltipClient {
public:
    int cc = -1;
    std::function<void(int)> onMove;
    AVCCPill() { setMouseCursor(MouseCursor::UpDownResizeCursor); setRepaintsOnMouseActivity(true); }
    void paint(Graphics& g) override {
        const auto area = getLocalBounds().toFloat();
        AVDexedTheme::fillBox(g, area, 9.0f, isMouseOver(true));
        AVDexedTheme::drawOceanText(g, cc >= 0 ? "CC " + String(cc) : String::fromUTF8("—"), AVDexedTheme::font(15.0f), area);
    }
    void mouseDown(const MouseEvent&) override { AVStartCC = cc; }
    void mouseDrag(const MouseEvent& event) override {
        if (cc < 0 || !onMove) return;
        const int steps = -event.getDistanceFromDragStartY() / 6;
        int target = AVStartCC, direction = steps >= 0 ? 1 : -1;
        for (int i = 0; i < std::abs(steps); ++i) {
            target += direction;
            if (target == 64) target += direction;
        }
        target = jlimit(0, 127, target);
        if (target != cc) onMove(target);
    }
private:
    int AVStartCC = -1;
};

/** One controller assignment: parameter · CC pill · Invert · Unassign. */
class AVMappingRow final : public Component {
public:
    String parameterId, title;
    AVCCPill pill;
    TextButton invert, unassign;
    AVMappingRow() {
        addAndMakeVisible(pill);
        for (auto* button : { &invert, &unassign }) { button->setWantsKeyboardFocus(true); addAndMakeVisible(button); }
    }
    void paint(Graphics& g) override {
        AVDexedTheme::drawGreyText(g, title, AVDexedTheme::font(15.0f), { 0.0f, 0.0f, 300.0f, float(getHeight()) },
                                   Justification::centredLeft);
    }
    // GOZO: CC 60, Invert 84, Unassign 96, 8–12 px apart; the parameter
    // column is 300 here (GOZO 220) because DX parameter names run longer.
    void resized() override {
        pill.setBounds(304, 0, 60, getHeight());
        invert.setBounds(372, 0, 84, getHeight());
        unassign.setBounds(468, 0, 96, getHeight());
    }
};
}

/** Audio, as in GOZO: Local BPM, then the output device with Test, sample
    rate and buffer size, all as combo boxes (no channel check boxes: the first
    stereo pair is used). Plugins keep only the BPM; the host owns the device. */
class AVDexedSettingsPanel::AVAudioPage final : public Component, private ChangeListener {
public:
    explicit AVAudioPage(DexedAudioProcessor& processor) : AVManager(AVDexedStandaloneAccess::AVDeviceManager) {
        AVTempo.setSliderStyle(Slider::LinearHorizontal);
        AVTempo.setTextBoxStyle(Slider::TextBoxLeft, false, int(AVDexedSettingsStyle::valueWidth), 28);
        AVTempo.setWantsKeyboardFocus(true);
        addAndMakeVisible(AVTempo);
        AVTempoAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(
            processor.parameters, IDs::avDelayTempo.name, AVTempo);
        // GOZO's BPM fader spans 30–300 in whole beats; the value reads "120".
        AVTempo.setRange(30.0, 300.0, 1.0);
        AVTempo.textFromValueFunction = [](double value) { return String(roundToInt(value)); };
        AVTempo.updateText();
        AVTempo.setDoubleClickReturnValue(true, 120.0);
        if (AVManager != nullptr) {
            for (auto* box : { &AVType, &AVOutput, &AVRate, &AVBuffer }) {
                box->setWantsKeyboardFocus(true);
                addAndMakeVisible(box);
            }
            AVTest.setWantsKeyboardFocus(true);
            addAndMakeVisible(AVTest);
            AVType.onChange = [this] {
                if (AVType.getSelectedId() > 0 && AVType.getText() != AVManager->getCurrentAudioDeviceType())
                    AVManager->setCurrentAudioDeviceType(AVType.getText(), true);
                AVRefreshDevices();
            };
            AVOutput.onChange = [this] {
                if (AVOutput.getSelectedId() <= 0) return;
                const auto name = AVOutput.getText();
                AVApply([name](AudioDeviceManager::AudioDeviceSetup& setup) {
                    setup.outputDeviceName = name;
                    setup.useDefaultOutputChannels = true;
                });
            };
            AVRate.onChange = [this] {
                const int index = AVRate.getSelectedId() - 1;
                if (!isPositiveAndBelow(index, AVRates.size())) return;
                const double rate = AVRates[index];
                AVApply([rate](AudioDeviceManager::AudioDeviceSetup& setup) { setup.sampleRate = rate; });
            };
            AVBuffer.onChange = [this] {
                const int index = AVBuffer.getSelectedId() - 1;
                if (!isPositiveAndBelow(index, AVBuffers.size())) return;
                const int size = AVBuffers[index];
                AVApply([size](AudioDeviceManager::AudioDeviceSetup& setup) { setup.bufferSize = size; });
            };
            AVTest.onClick = [this] { AVManager->playTestSound(); };
            AVManager->addChangeListener(this);
        }
        AVRefreshLanguage();
    }
    ~AVAudioPage() override {
        if (AVManager != nullptr) AVManager->removeChangeListener(this);
        AVTempoAttachment.reset();
    }
    void AVRefreshLanguage() {
        auto& language = AVDexedLanguageService::getInstance();
        AVTempo.setTitle(language.text("localBpm"));
        AVTempo.setTooltip(language.text("localTempoHelp"));
        AVType.setTitle(language.text("audioDeviceTypeShort"));
        AVOutput.setTitle(language.text("audioOutputDevice"));
        AVRate.setTitle(language.text("audioSampleRate"));
        AVBuffer.setTitle(language.text("audioBufferSize"));
        AVOutput.setTextWhenNoChoicesAvailable(language.text("audioNoOutput"));
        AVTest.setButtonText(language.text("audioTest"));
        AVTest.setTitle(AVTest.getButtonText());
        AVTest.setTooltip(language.text("audioTestHelp"));
        AVRefreshDevices();
        resized();
        repaint();
    }
    void resized() override {
        namespace S = AVDexedSettingsStyle;
        const int x = int(S::fieldX), width = jmin(440, getWidth() - x);
        const auto row = [](int index) { return roundToInt(S::rowGap * float(index)); };
        AVTempo.setBounds(x, row(0), width, int(S::rowHeight));
        AVType.setBounds(x, row(1), width, int(S::rowHeight));
        const int test = S::pillWidth(AVTest.getButtonText(), 60);
        AVOutput.setBounds(x, row(2), width - test - 10, int(S::rowHeight));
        AVTest.setBounds(x + width - test, row(2), test, int(S::rowHeight));
        AVRate.setBounds(x, row(3), width, int(S::rowHeight));
        AVBuffer.setBounds(x, row(4), width, int(S::rowHeight));
    }
    void paint(Graphics& g) override {
        namespace S = AVDexedSettingsStyle;
        auto& language = AVDexedLanguageService::getInstance();
        const auto label = [&g](const String& text, int index) {
            S::label(g, text, { 0.0f, S::rowGap * float(index), S::labelWidth, S::rowHeight });
        };
        label(language.text("localBpm"), 0);
        if (AVManager == nullptr) {
            S::hintParagraph(g, language.text("audioHostHelp"), { 0.0f, S::rowGap * 2.0f, float(getWidth()), 60.0f });
            return;
        }
        if (AVType.isVisible()) label(language.text("audioDeviceTypeShort"), 1);
        label(language.text("audioOutputDevice"), 2);
        label(language.text("audioSampleRate"), 3);
        label(language.text("audioBufferSize"), 4);
    }

private:
    AudioDeviceManager* AVManager = nullptr;
    Slider AVTempo;
    std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> AVTempoAttachment;
    ComboBox AVType, AVOutput, AVRate, AVBuffer;
    TextButton AVTest;
    Array<double> AVRates;
    Array<int> AVBuffers;

    void AVApply(const std::function<void(AudioDeviceManager::AudioDeviceSetup&)>& change) {
        auto setup = AVManager->getAudioDeviceSetup();
        change(setup);
        const auto error = AVManager->setAudioDeviceSetup(setup, true);
        AVRefreshDevices();
        if (error.isNotEmpty()) {
            auto& language = AVDexedLanguageService::getInstance();
            AVShowMessage(AlertWindow::WarningIcon, language.text("audioPage"), language.text("audioDeviceError") + "\n" + error, this);
        }
    }

    void AVRefreshDevices() {
        if (AVManager == nullptr) return;
        auto& language = AVDexedLanguageService::getInstance();
        // Driver: only shown where there is a real choice (the GOZO page keeps
        // that row empty; on macOS there is just Core Audio).
        const auto& types = AVManager->getAvailableDeviceTypes();
        AVType.clear(dontSendNotification);
        for (int index = 0; index < types.size(); ++index) {
            AVType.addItem(types[index]->getTypeName(), index + 1);
            if (types[index]->getTypeName() == AVManager->getCurrentAudioDeviceType())
                AVType.setSelectedId(index + 1, dontSendNotification);
        }
        AVType.setVisible(types.size() > 1);

        const auto setup = AVManager->getAudioDeviceSetup();
        AVOutput.clear(dontSendNotification);
        if (auto* type = AVManager->getCurrentDeviceTypeObject()) {
            const auto names = type->getDeviceNames(false);
            for (int index = 0; index < names.size(); ++index)
                AVOutput.addItem(names[index], index + 1);
            AVOutput.setSelectedId(names.indexOf(setup.outputDeviceName) + 1, dontSendNotification);
        }

        AVRate.clear(dontSendNotification);
        AVBuffer.clear(dontSendNotification);
        AVRates.clear();
        AVBuffers.clear();
        auto* device = AVManager->getCurrentAudioDevice();
        if (device != nullptr) {
            AVRates = device->getAvailableSampleRates();
            const double rate = device->getCurrentSampleRate();
            for (int index = 0; index < AVRates.size(); ++index) {
                AVRate.addItem(String(roundToInt(AVRates[index])) + " Hz", index + 1);
                if (approximatelyEqual(AVRates[index], rate)) AVRate.setSelectedId(index + 1, dontSendNotification);
            }
            AVBuffers = device->getAvailableBufferSizes();
            const int current = device->getCurrentBufferSizeSamples();
            for (int index = 0; index < AVBuffers.size(); ++index) {
                const int size = AVBuffers[index];
                AVBuffer.addItem(String(size) + " " + language.text("audioSamplesUnit") + " ("
                                     + String(size * 1000.0 / jmax(1.0, rate), 1) + " ms)", index + 1);
                if (size == current) AVBuffer.setSelectedId(index + 1, dontSendNotification);
            }
        }
        for (auto* control : std::initializer_list<Component*> { &AVRate, &AVBuffer, &AVTest })
            control->setEnabled(device != nullptr);
        repaint();
    }

    void changeListenerCallback(ChangeBroadcaster*) override { AVRefreshDevices(); }
};

/** MIDI, as in GOZO: enabled inputs (Standalone) and the controller (CC) list. */
class AVDexedSettingsPanel::AVMidiPage final : public Component, private Timer, private ChangeListener {
public:
    explicit AVMidiPage(DexedAudioProcessor& processor) : AVProcessor(processor) {
        addAndMakeVisible(AVViewport);
        AVViewport.setViewedComponent(&AVContent, false);
        AVThumb = std::make_unique<AVScrollThumb>(AVViewport);
        addChildComponent(*AVThumb);
        AVContent.painter = [this](Graphics& g) { AVPaintContent(g); };
        AVContent.addAndMakeVisible(AVParameter);
        AVContent.addAndMakeVisible(AVLearn);
        AVParameter.setWantsKeyboardFocus(true);
        AVLearn.setWantsKeyboardFocus(true);
        AVLearn.onClick = [this] { AVLearningParameter.isEmpty() ? AVStartLearn() : AVCancelLearn(); };
        AVDexedLanguageService::getInstance().addChangeListener(this);
        AVRefreshLanguage();
        AVRefresh(true);
        startTimerHz(4);
    }
    ~AVMidiPage() override {
        stopTimer();
        AVDexedLanguageService::getInstance().removeChangeListener(this);
        AVCancelLearn();
    }
    void AVRefreshRuntime() { AVRefresh(true); }
    void resized() override {
        AVViewport.setBounds(getLocalBounds().withTrimmedRight(12));
        AVThumb->setBounds(getWidth() - 12, 0, 12, getHeight());
        AVLayout();
    }

private:
    DexedAudioProcessor& AVProcessor;
    Viewport AVViewport;
    std::unique_ptr<AVScrollThumb> AVThumb;
    AVPaintedContent AVContent;
    std::vector<std::unique_ptr<AVInputToggleRow>> AVInputs;
    std::vector<std::unique_ptr<AVMappingRow>> AVRows;
    Array<MidiDeviceInfo> AVKnownInputs;
    std::vector<std::tuple<int, String, bool>> AVKnownRows;
    ComboBox AVParameter;
    TextButton AVLearn;
    StringArray AVParameterIds;
    String AVLearningParameter, AVStatus;
    int AVCcTop = 0, AVAddTop = 0;

    // GOZO MIDI page: inputs every 28 px from y 30; the CC table starts 46 px
    // below them (heading 30 above), rows 26 tall every 30.
    static constexpr int AVRowHeight = 28, AVCcRowPitch = 30, AVCcRowHeight = 26;

    void AVPaintContent(Graphics& g) {
        namespace S = AVDexedSettingsStyle;
        auto& language = AVDexedLanguageService::getInstance();
        const float width = float(AVContent.getWidth());
        S::heading(g, language.text("midiInputsTitle"), { 0.0f, 0.0f, width, 24.0f });
        if (AVDexedStandaloneAccess::AVDeviceManager == nullptr || AVInputs.empty())
            S::hint(g, language.text(AVDexedStandaloneAccess::AVDeviceManager == nullptr ? "midiHostInputs" : "audioNoMidiInputs"),
                    { 0.0f, 30.0f, width, 30.0f });
        S::heading(g, language.text("midiCcTitle"), { 0.0f, float(AVCcTop - 30), width, 24.0f });
        S::hint(g, language.text("midiParameter"), { 0.0f, float(AVCcTop - 2), 300.0f, 20.0f });
        S::hint(g, "CC", { 308.0f, float(AVCcTop - 2), 52.0f, 20.0f }, Justification::centred);
        if (AVRows.empty())
            S::hint(g, language.text("noMappingsHint"), { 0.0f, float(AVCcTop + 22), width, 26.0f });
        S::hint(g, language.text("addMapping"), { 0.0f, float(AVAddTop - 22), 300.0f, 20.0f });
        if (AVStatus.isNotEmpty()) {
            const auto area = Rectangle<float>(0.0f, float(AVAddTop + 34), width, 24.0f);
            if (AVLearningParameter.isNotEmpty()) AVDexedTheme::drawOceanText(g, AVStatus, AVDexedTheme::font(13.0f), area, Justification::centredLeft);
            else S::hint(g, AVStatus, area);
        }
    }

    void AVLayout() {
        const int width = jmax(200, AVViewport.getWidth());
        int y = 30;
        for (auto& row : AVInputs) { row->setBounds(0, y, width, AVRowHeight); y += AVRowHeight; }
        AVCcTop = 30 + jmax(1, int(AVInputs.size())) * AVRowHeight + 46;
        y = AVCcTop + 22;
        for (auto& row : AVRows) { row->setBounds(0, y, width, AVCcRowHeight); y += AVCcRowPitch; }
        if (AVRows.empty()) y += AVCcRowPitch;
        AVAddTop = y + 34;
        AVParameter.setBounds(0, AVAddTop, 300, AVCcRowHeight);
        AVLearn.setBounds(308, AVAddTop, AVDexedSettingsStyle::pillWidth(AVLearn.getButtonText(), 156), AVCcRowHeight);
        AVContent.setSize(width, AVAddTop + 64);
        AVContent.repaint();
        AVThumb->update();
    }

    void AVRefresh(bool force) {
        auto& language = AVDexedLanguageService::getInstance();
        bool changed = force;
        if (auto* devices = AVDexedStandaloneAccess::AVDeviceManager) {
            const auto available = MidiInput::getAvailableDevices();
            if (available != AVKnownInputs || force) {
                AVKnownInputs = available;
                AVInputs.clear();
                for (const auto& device : available) {
                    auto row = std::make_unique<AVInputToggleRow>();
                    row->name = device.name;
                    row->setTitle(device.name);
                    const auto identifier = device.identifier;
                    row->onToggle = [this, identifier] {
                        if (auto* manager = AVDexedStandaloneAccess::AVDeviceManager) {
                            manager->setMidiInputDeviceEnabled(identifier, !manager->isMidiInputDeviceEnabled(identifier));
                            AVRefreshInputStates();
                        }
                    };
                    AVContent.addAndMakeVisible(*row);
                    AVInputs.push_back(std::move(row));
                }
                changed = true;
            }
            AVRefreshInputStates();
        }
        std::vector<std::tuple<int, String, bool>> mappings;
        for (int cc = 0; cc < 128; ++cc) {
            const auto id = AVProcessor.midiCCMapper->getAVMappingParameterId(cc);
            if (id.isNotEmpty()) mappings.emplace_back(cc, id, AVProcessor.midiCCMapper->isAVInverted(cc));
        }
        if (mappings != AVKnownRows || force) {
            AVKnownRows = mappings;
            AVRows.clear();
            for (const auto& [cc, id, inverted] : mappings) {
                auto row = std::make_unique<AVMappingRow>();
                auto* parameter = AVProcessor.parameters.getParameter(id);
                row->parameterId = id;
                row->title = language.parameterName(id, parameter != nullptr ? parameter->getName(100) : id);
                row->pill.cc = cc;
                row->pill.setTooltip(language.text("ccPillHelp"));
                row->pill.setTitle(row->title + " · CC " + String(cc));
                const int fromCC = cc;
                row->pill.onMove = [this, fromCC](int toCC) {
                    if (AVProcessor.midiCCMapper->moveAVMapping(fromCC, toCC)) AVRefresh(false);
                };
                row->invert.setButtonText(language.text("invert"));
                row->invert.setTooltip(language.text("invertHelp"));
                row->invert.setToggleState(inverted, dontSendNotification);
                row->invert.onClick = [this, fromCC] {
                    AVProcessor.midiCCMapper->setAVInverted(fromCC, !AVProcessor.midiCCMapper->isAVInverted(fromCC));
                    AVRefresh(false);
                };
                row->unassign.setButtonText(language.text("unassign"));
                row->unassign.onClick = [this, fromCC] {
                    AVProcessor.midiCCMapper->removeMapping(fromCC);
                    AVRefresh(false);
                };
                AVContent.addAndMakeVisible(*row);
                AVRows.push_back(std::move(row));
            }
            changed = true;
        }
        if (changed) AVLayout();
    }

    void AVRefreshInputStates() {
        auto* manager = AVDexedStandaloneAccess::AVDeviceManager;
        for (size_t i = 0; i < AVInputs.size() && i < size_t(AVKnownInputs.size()); ++i) {
            const bool on = manager != nullptr && manager->isMidiInputDeviceEnabled(AVKnownInputs[int(i)].identifier);
            if (AVInputs[i]->on != on) { AVInputs[i]->on = on; AVInputs[i]->repaint(); }
        }
    }

    void AVStartLearn() {
        if (!isPositiveAndBelow(AVParameter.getSelectedItemIndex(), AVParameterIds.size())) return;
        const auto id = AVParameterIds[AVParameter.getSelectedItemIndex()];
        auto* parameter = AVProcessor.parameters.getParameter(id);
        if (parameter == nullptr) return;
        AVLearningParameter = id;
        const auto safe = Component::SafePointer<AVMidiPage>(this);
        AVProcessor.midiCCMapper->startMapping(parameter, [safe, id] {
            if (safe == nullptr) return;
            auto& language = AVDexedLanguageService::getInstance();
            auto* learned = safe->AVProcessor.parameters.getParameter(id);
            safe->AVStatus = language.format("learnComplete", {{"cc", String(safe->AVProcessor.midiCCMapper->getAVLastLearnedCC())},
                {"parameter", language.parameterName(id, learned != nullptr ? learned->getName(100) : id)}});
            safe->AVLearningParameter.clear();
            safe->AVLearn.setButtonText(language.text("midiLearn"));
            safe->AVRefresh(false);
            safe->AVContent.repaint();
        });
        AVStatus = AVDexedLanguageService::getInstance().text("learnWaiting");
        AVLearn.setButtonText(AVDexedLanguageService::getInstance().text("cancel"));
        AVContent.repaint();
    }

    void AVCancelLearn() {
        if (AVLearningParameter.isEmpty()) return;
        if (AVProcessor.midiCCMapper->getAVLearningParameterId() == AVLearningParameter)
            AVProcessor.midiCCMapper->cancelMapping();
        AVLearningParameter.clear();
        AVStatus.clear();
        AVLearn.setButtonText(AVDexedLanguageService::getInstance().text("midiLearn"));
        AVContent.repaint();
    }

    void AVRefreshLanguage() {
        auto& language = AVDexedLanguageService::getInstance();
        const int selected = jmax(1, AVParameter.getSelectedId());
        AVParameter.clear(dontSendNotification);
        AVParameterIds.clear();
        for (auto* parameter : AVProcessor.getParameters())
            if (auto* ranged = dynamic_cast<RangedAudioParameter*>(parameter)) {
                const auto id = ranged->getParameterID();
                AVParameterIds.add(id);
                AVParameter.addItem(language.parameterName(id, parameter->getName(100)), AVParameterIds.size());
            }
        AVParameter.setSelectedId(selected, dontSendNotification);
        AVParameter.setTitle(language.text("midiParameter"));
        AVLearn.setButtonText(language.text(AVLearningParameter.isEmpty() ? "midiLearn" : "cancel"));
        AVLearn.setTitle(language.text("midiLearn"));
        AVRefresh(true);
    }

    void timerCallback() override { if (isShowing()) AVRefresh(false); }
    void changeListenerCallback(ChangeBroadcaster*) override { AVRefreshLanguage(); }
};

/** SysEx: the dedicated port pair and channel used to exchange voices/banks. */
class AVDexedSettingsPanel::AVSysExPage final : public Component, private ChangeListener {
public:
    explicit AVSysExPage(DexedAudioProcessor& processor) : AVProcessor(processor) {
        for (auto* box : {&AVInput, &AVOutput}) {
            box->setWantsKeyboardFocus(true);
            addAndMakeVisible(box);
        }
        AVInput.addItem("None", 1);
        AVOutput.addItem("None", 1);
        for (const auto& device : MidiInput::getAvailableDevices()) AVInput.addItem(device.name, AVInput.getNumItems() + 1);
        for (const auto& device : MidiOutput::getAvailableDevices()) AVOutput.addItem(device.name, AVOutput.getNumItems() + 1);
        AVInput.onChange = [this] { AVChangePort(true); };
        AVOutput.onChange = [this] { AVChangePort(false); };
        AVChannel.setRange(1, 16, 1);
        AVChannel.setSliderStyle(Slider::LinearHorizontal);
        AVChannel.setTextBoxStyle(Slider::TextBoxLeft, false, int(AVDexedSettingsStyle::valueWidth), 28);
        AVChannel.onValueChange = [this] { AVProcessor.sysexComm.setChl(int(AVChannel.getValue()) - 1); AVProcessor.savePreference(); };
        AVChannel.setWantsKeyboardFocus(true);
        addAndMakeVisible(AVChannel);
        AVSend.onClick = [this] { AVProcessor.sendCurrentSysexProgram(); };
        AVSend.setWantsKeyboardFocus(true);
        addAndMakeVisible(AVSend);
        AVDexedLanguageService::getInstance().addChangeListener(this);
        AVRefreshLanguage();
        AVRefreshRuntime();
    }
    ~AVSysExPage() override { AVDexedLanguageService::getInstance().removeChangeListener(this); }
    void AVRefreshRuntime() {
        AVSelectPort(AVInput, AVProcessor.sysexComm.getInput());
        AVSelectPort(AVOutput, AVProcessor.sysexComm.getOutput());
        AVChannel.setValue(AVProcessor.sysexComm.getChl() + 1, dontSendNotification);
        AVSend.setEnabled(AVProcessor.sysexComm.isOutputActive());
    }
    // Same rows as Audio (GOZO): labels right-aligned in 0..190, fields from 202.
    void resized() override {
        namespace S = AVDexedSettingsStyle;
        const int x = int(S::fieldX), width = jmin(440, getWidth() - x);
        AVInput.setBounds(x, 0, width, int(S::rowHeight));
        AVOutput.setBounds(x, int(S::rowGap), width, int(S::rowHeight));
        AVChannel.setBounds(x, int(S::rowGap * 2.0f), width, int(S::rowHeight));
        AVSend.setBounds(x, int(S::rowGap * 3.0f) + 6, S::pillWidth(AVSend.getButtonText(), 160), int(S::rowHeight));
    }
    void paint(Graphics& g) override {
        namespace S = AVDexedSettingsStyle;
        auto& language = AVDexedLanguageService::getInstance();
        const auto label = [&g](const String& text, int index) {
            S::label(g, text + ":", { 0.0f, S::rowGap * float(index), S::labelWidth, S::rowHeight });
        };
        label(language.text("midiInputs"), 0);
        label(language.text("midiOutputs"), 1);
        label(language.text("sysexChannel"), 2);
        S::hintParagraph(g, language.text("sysexHelp"), { 0.0f, S::rowGap * 4.0f + 24.0f, float(getWidth()), 60.0f });
    }
private:
    DexedAudioProcessor& AVProcessor;
    ComboBox AVInput, AVOutput;
    Slider AVChannel;
    TextButton AVSend;
    static void AVSelectPort(ComboBox& box, const String& name) {
        int id = 1;
        for (int index = 1; index < box.getNumItems(); ++index)
            if (box.getItemText(index) == name) id = index + 1;
        box.setSelectedId(id, dontSendNotification);
    }
    void AVChangePort(bool input) {
        auto& box = input ? AVInput : AVOutput;
        const auto device = box.getSelectedId() == 1 ? String() : box.getText();
        const bool success = input ? AVProcessor.sysexComm.setInput(device) : AVProcessor.sysexComm.setOutput(device);
        if (success) {
            AVProcessor.savePreference();
            AVSend.setEnabled(AVProcessor.sysexComm.isOutputActive());
            return;
        }
        AVSelectPort(box, input ? AVProcessor.sysexComm.getInput() : AVProcessor.sysexComm.getOutput());
        auto& language = AVDexedLanguageService::getInstance();
        AVShowMessage(AlertWindow::WarningIcon, language.text("midiInterface"), language.text("midiPortsError"), this);
    }
    void AVRefreshLanguage() {
        auto& language = AVDexedLanguageService::getInstance();
        const int input = AVInput.getSelectedId(), output = AVOutput.getSelectedId();
        AVInput.changeItemText(1, language.text("none"));
        AVOutput.changeItemText(1, language.text("none"));
        AVInput.setSelectedId(input, dontSendNotification);
        AVOutput.setSelectedId(output, dontSendNotification);
        AVInput.setTitle(language.text("midiInputs"));
        AVOutput.setTitle(language.text("midiOutputs"));
        AVChannel.setTitle(language.text("sysexChannel"));
        AVSend.setButtonText(language.text("sendCurrentVoice"));
        AVSend.setTitle(AVSend.getButtonText());
        repaint();
    }
    void changeListenerCallback(ChangeBroadcaster*) override { AVRefreshLanguage(); }
};

/** About, as in GOZO: logo top-left (40 tall), version line, the text, link
    pills at the bottom and the language selector top-right. */
class AVDexedSettingsPanel::AVAboutPage final : public Component {
public:
    AVAboutPage() {
        if (auto xml = XmlDocument::parse(String::fromUTF8(BinaryData::AVDexed_Logo_svg, BinaryData::AVDexed_Logo_svgSize)))
            AVLogo = Drawable::createFromSVG(*xml);
        AVOriginal.onClick = [] { URL("https://asb2m10.github.io/dexed/").launchInDefaultBrowser(); };
        AVSurge.onClick = [] { URL("https://surge-synthesizer.github.io/").launchInDefaultBrowser(); };
        for (auto* link : { &AVOriginal, &AVSurge }) {
            link->setWantsKeyboardFocus(true);
            addAndMakeVisible(link);
        }
        AVLanguage.addItem("English", 1);
        AVLanguage.addItem(String::fromUTF8("Español"), 2);
        AVLanguage.setWantsKeyboardFocus(true);
        AVLanguage.onChange = [this] {
            AVDexedLanguageService::getInstance().setLanguage(AVLanguage.getSelectedId() == 2 ? "es" : "en");
        };
        addAndMakeVisible(AVLanguage);
        AVRefreshLanguage();
    }
    void AVRefreshLanguage() {
        auto& language = AVDexedLanguageService::getInstance();
        AVOriginal.setButtonText(language.text("originalProject"));
        AVOriginal.setTitle(AVOriginal.getButtonText());
        AVOriginal.setTooltip("asb2m10.github.io/dexed");
        AVSurge.setButtonText(language.text("surgeProject"));
        AVSurge.setTitle(AVSurge.getButtonText());
        AVSurge.setTooltip("surge-synthesizer.github.io");
        AVLanguage.setSelectedId(language.isSpanish() ? 2 : 1, dontSendNotification);
        AVLanguage.setTitle(language.text("languageLabel"));
        resized();
        repaint();
    }
    void resized() override {
        AVLanguage.setBounds(getWidth() - 118, 0, 118, 32);
        const int original = AVDexedSettingsStyle::pillWidth(AVOriginal.getButtonText(), 210);
        AVOriginal.setBounds(0, getHeight() - 44, original, 32);
        AVSurge.setBounds(original + 12, getHeight() - 44, AVDexedSettingsStyle::pillWidth(AVSurge.getButtonText(), 210), 32);
    }
    void paint(Graphics& g) override {
        auto& language = AVDexedLanguageService::getInstance();
        if (AVLogo != nullptr)
            AVLogo->drawWithin(g, { 0.0f, 0.0f, 260.0f, 40.0f }, RectanglePlacement::xLeft | RectanglePlacement::yTop, 1.0f);
        AVDexedTheme::drawGreyText(g, language.text("languageLabel") + ":", AVDexedTheme::font(13.0f),
                                   { float(getWidth() - 205), 0.0f, 82.0f, 32.0f }, Justification::centredRight, 0.8f);
        AVDexedTheme::drawGreyText(g, language.format("aboutVersion", {{"version", DEXED_VERSION}}) + String::fromUTF8("   —   ")
                                       + language.text("aboutBy"),
                                   AVDexedTheme::font(13.0f), { 0.0f, 60.0f, float(getWidth()), 20.0f }, Justification::centredLeft, 0.8f);
        // Body Regular 15 at 90% on GOZO's 22 px rhythm; credits as hints.
        const auto body = AVDexedTheme::textPrimary.interpolatedWith(AVDexedTheme::textSecondary, 0.4f);
        AttributedString text;
        text.setLineSpacing(4.0f);
        text.append(language.text("aboutDescription") + "\n\n", AVDexedTheme::font(15.0f), body.withAlpha(0.9f));
        text.append(language.text("aboutThanks") + "\n\n", AVDexedTheme::font(15.0f), body.withAlpha(0.9f));
        text.append(language.text("aboutCredits"), AVDexedTheme::font(13.0f), body.withAlpha(0.7f));
        TextLayout layout;
        layout.createLayout(text, float(getWidth()));
        layout.draw(g, { 0.0f, 96.0f, float(getWidth()), float(getHeight() - 96 - 56) });
    }
private:
    std::unique_ptr<Drawable> AVLogo;
    TextButton AVOriginal, AVSurge;
    ComboBox AVLanguage;
};

AVDexedSettingsPanel::AVDexedSettingsPanel(DexedAudioProcessor& processor, AVCallbacks callbacks)
    : AVProcessor(processor), AVActions(std::move(callbacks)) {
    setLookAndFeel(&AVLookAndFeel);
    setSize(1180, 650);
    setWantsKeyboardFocus(true);
    setFocusContainerType(FocusContainerType::focusContainer);
    for (size_t index = 0; index < AVTabs.size(); ++index) {
        AVTabs[index] = std::make_unique<AVTabButton>(static_cast<int>(index));
        addAndMakeVisible(*AVTabs[index]);
        AVTabs[index]->onClick = [this, index] { AVShowPage(static_cast<AVPage>(index)); };
    }
    addAndMakeVisible(AVClose);
    AVClose.setWantsKeyboardFocus(true);
    int AVCloseSvgSize = 0;
    if (const auto* AVCloseSvg = BinaryData::getNamedResource("AVDexed_CloseIcon_svg", AVCloseSvgSize))
        if (auto xml = XmlDocument::parse(String::fromUTF8(AVCloseSvg, AVCloseSvgSize)))
            if (auto icon = Drawable::createFromSVG(*xml)) {
                auto hover = icon->createCopy();
                hover->replaceColour(AVDexedTheme::textPrimary, AVDexedTheme::oceanHighlight);
                AVClose.setImages(icon.get(), hover.get(), hover.get());
            }
    AVClose.setColour(DrawableButton::backgroundColourId, Colours::transparentBlack);
    AVClose.setColour(DrawableButton::backgroundOnColourId, Colours::transparentBlack);
    AVClose.onClick = [this] { if (AVActions.close) AVActions.close(); };
    AVInstrument = std::make_unique<ParamDialog>(processor.parameters, processor);
    AVInstrument->setDialogValues(processor.controllers, processor.sysexComm, processor.showKeyboard, processor.getZoomFactor());
    AVInstrument->setIsStandardTuning(processor.synthTuningState->is_standard_tuning());
    AVInstrument->setGeneralCallback(AVActions.applyInstrumentSettings ? AVActions.applyInstrumentSettings : [](ParamDialog*) {});
    AVInstrument->setTuningCallback(AVActions.tuningAction ? AVActions.tuningAction : [](ParamDialog*, ParamDialog::TuningAction) {});
    addAndMakeVisible(*AVInstrument);
    AVAudio = std::make_unique<AVAudioPage>(processor);
    addAndMakeVisible(*AVAudio);
    AVAbout = std::make_unique<AVAboutPage>();
    addAndMakeVisible(*AVAbout);
    if (AVActions.createMidiContent) AVMidi = AVActions.createMidiContent();
    else AVMidi = std::make_unique<AVMidiPage>(processor);
    if (AVMidi) addAndMakeVisible(*AVMidi);
    AVSysEx = std::make_unique<AVSysExPage>(processor);
    addAndMakeVisible(*AVSysEx);
    AVMidiHostNotice.setFont(AVDexedTheme::font(13.0f, "Regular"));
    AVMidiHostNotice.setJustificationType(Justification::topLeft);
    AVMidiHostNotice.setColour(Label::textColourId, AVDexedTheme::textPrimary);
    addChildComponent(AVMidiHostNotice);
    AVDexedLanguageService::getInstance().addChangeListener(this);
    AVRefreshLanguage();
    AVShowPage(AVPage::audio);
    // Pages build their controls before joining this panel; re-run the look so
    // value boxes and pills come from the Settings style, not the default.
    sendLookAndFeelChange();
    resized();
}

AVDexedSettingsPanel::~AVDexedSettingsPanel() {
    AVDexedLanguageService::getInstance().removeChangeListener(this);
    setLookAndFeel(nullptr);
}

void AVDexedSettingsPanel::AVShowPage(AVPage page) {
    AVSelectedPage = page;
    if (page == AVPage::instrument)
        AVInstrument->setDialogValues(AVProcessor.controllers, AVProcessor.sysexComm, AVProcessor.showKeyboard, AVProcessor.getZoomFactor());
    if (auto* midi = dynamic_cast<AVMidiPage*>(AVMidi.get()))
        if (page == AVPage::midi) midi->AVRefreshRuntime();
    if (page == AVPage::sysex) AVSysEx->AVRefreshRuntime();
    for (size_t index = 0; index < AVTabs.size(); ++index)
        AVTabs[index]->setToggleState(index == static_cast<size_t>(page), dontSendNotification);
    AVAudio->setVisible(page == AVPage::audio);
    AVInstrument->setVisible(page == AVPage::instrument);
    AVAbout->setVisible(page == AVPage::about);
    AVSysEx->setVisible(page == AVPage::sysex);
    if (AVMidi) AVMidi->setVisible(page == AVPage::midi);
    AVMidiHostNotice.setVisible(!AVMidi && page == AVPage::midi);
    PopupMenu::dismissAllActiveMenus();
    repaint();
}

void AVDexedSettingsPanel::AVRefreshLanguage() {
    auto& language = AVDexedLanguageService::getInstance();
    const char* keys[] {"audioPage", "instrumentPage", "midiPage", "sysexPage", "aboutPage"};
    for (size_t index = 0; index < AVTabs.size(); ++index) {
        AVTabs[index]->setButtonText(language.text(keys[index]));
        AVTabs[index]->setTitle(AVTabs[index]->getButtonText());
    }
    AVClose.setTitle(language.text("close"));
    AVClose.setTooltip(language.text("close"));
    AVAudio->AVRefreshLanguage();
    AVAbout->AVRefreshLanguage();
    AVMidiHostNotice.setText(language.text("midiHostHelp"), dontSendNotification);
    setTitle(language.text("settingsTitle"));
    repaint();
}

void AVDexedSettingsPanel::changeListenerCallback(ChangeBroadcaster*) { AVRefreshLanguage(); }

void AVDexedSettingsPanel::paint(Graphics& g) {
    AVDexedTheme::panel(g, getLocalBounds().toFloat(), 12.0f);
    // Title band: the same vertical gradient over its own 60 px (GOZO). The
    // window is rounded, so only its top corners follow; the bottom is straight.
    const auto title = Rectangle<float>(0, 0, static_cast<float>(getWidth()), 60);
    Path band;
    band.addRoundedRectangle(title.getX(), title.getY(), title.getWidth(), title.getHeight(), 12.0f, 12.0f,
                             true, true, false, false);
    g.setGradientFill(ColourGradient(AVDexedTheme::panelTop, 0, 0, AVDexedTheme::panelBottom, 0, title.getBottom(), false));
    g.fillPath(band);
    AVDexedTheme::drawGreyText(g, AVDexedLanguageService::getInstance().text("settingsTitle"),
                               AVDexedTheme::font(30.0f, "SemiBold"), title, Justification::centred);
    const auto separator = Rectangle<float>(285, 86, 1, static_cast<float>(getHeight() - 140));
    g.setGradientFill(ColourGradient(AVDexedTheme::textPrimary, separator.getX(), separator.getY(),
                                     AVDexedTheme::textSecondary, separator.getX(), separator.getBottom(), false));
    g.fillRoundedRectangle(separator, 0.5f);
}

// GOZO geometry: 19 px close X 28.5 px from the right, centred in the 60 px
// band (6 px of extra hit area around it); sidebar highlights 247×50 every
// 90 px from (14, 81); separator at x 285; content from (315, 86) with a
// 30 px right margin and 54 px below.
void AVDexedSettingsPanel::resized() {
    AVClose.setBounds(Rectangle<float>(float(getWidth()) - 28.5f - 19.0f, 20.5f, 19.0f, 19.0f).expanded(6.0f).toNearestInt());
    AVClose.setEdgeIndent(6);
    for (size_t index = 0; index < AVTabs.size(); ++index)
        if (AVTabs[index]) AVTabs[index]->setBounds(14, 81 + static_cast<int>(index) * 90, 247, 50);
    const auto content = Rectangle<int>(315, 86, getWidth() - 345, getHeight() - 140);
    if (AVAudio) AVAudio->setBounds(content);
    if (AVInstrument) AVInstrument->setBounds(content);
    if (AVAbout) AVAbout->setBounds(content);
    if (AVMidi) AVMidi->setBounds(content);
    if (AVSysEx) AVSysEx->setBounds(content);
    AVMidiHostNotice.setBounds(content.reduced(8, 30));
}

bool AVDexedSettingsPanel::keyPressed(const KeyPress& key) {
    if (key.getKeyCode() != KeyPress::escapeKey) return false;
    if (AVActions.close) AVActions.close();
    return true;
}
