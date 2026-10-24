// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedLayersView.h"
#include "AVDexedLanguage.h"
#include "AVDexedTheme.h"
#include "AVDexedFade.h"
#include <cmath>
#include "../core/AVDexedSysex.h"
#include "../core/AVDexedLibrary.h"

using namespace juce;

namespace {
/** Note names as the on-screen keyboard counts octaves (C4 = 60). */
String noteName(int note) { return MidiMessage::getMidiNoteName(note, true, true, 4); }

String signedNumber(double value, int decimals) {
    if (std::abs(value) < (decimals > 0 ? 0.05 : 0.5)) return "0";
    return (value > 0 ? "+" : String::fromUTF8("−")) + String(std::abs(value), decimals);
}

String panText(double value) {
    if (std::abs(value - 0.5) < 0.005) return "C";
    return String(value < 0.5 ? "L " : "R ") + String(roundToInt(std::abs(value - 0.5) * 200.0));
}

String volumeText(double value) {
    return value <= 0.00001 ? String::fromUTF8("−∞ dB") : String(Decibels::gainToDecibels(value), 1) + " dB";
}

/** The operator pill of Advanced: "Sí"/"No", ocean letters when on. */
class LayerSwitch final : public ToggleButton {
public:
    void paintButton(Graphics& g, bool over, bool down) override {
        const auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        AVDexedTheme::fillBox(g, bounds, 9.0f, over && ! down && isEnabled(), 1.3f);
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

/** The layer's sound: a card like a selected list row; a click selects the
    layer, a right click offers the selected sound for it. */
class VoiceCard final : public Button {
public:
    VoiceCard() : Button("layerVoice") {
        setRepaintsOnMouseActivity(true);
        setWantsKeyboardFocus(true);
    }
    String name, voices;
    bool selected = false, sounding = false;
    std::function<void()> onContextMenu;

    void paintButton(Graphics& g, bool over, bool down) override {
        const auto area = getLocalBounds().toFloat().reduced(0.5f);
        if (selected) AVDexedTheme::fillRowHighlight(g, area, over ? 1.0f : 0.85f);
        else AVDexedTheme::fillBox(g, area, 9.0f, over && ! down && isEnabled());
        if (AVDexedTheme::showsFocus(*this)) {
            g.setColour(AVDexedTheme::oceanHighlight);
            g.drawRoundedRectangle(area, 9.0f, 1.2f);
        }
        auto text = area.reduced(10.0f, 6.0f);
        const auto nameArea = text.removeFromTop(text.getHeight() * 0.58f);
        const auto nameFont = AVDexedTheme::font(15.0f, "Medium");
        if (selected || over) AVDexedTheme::drawOceanText(g, name, nameFont, nameArea);
        else AVDexedTheme::drawGreyText(g, name, nameFont, nameArea);
        const auto small = AVDexedTheme::font(11.0f, "Medium");
        if (sounding) AVDexedTheme::drawOceanText(g, voices, small, text);
        else AVDexedTheme::drawGreyText(g, voices, small, text, Justification::centred, 0.6f);
    }
    void mouseDown(const MouseEvent& event) override {
        if (event.mods.isPopupMenu()) {
            if (onContextMenu) onContextMenu();
            return;
        }
        Button::mouseDown(event);
    }
};
}

//==============================================================================
class AVDexedLayersView::Strip final : public Component {
public:
    Strip(DexedAudioProcessor& owner, int index) : processor(owner), layer(index) {
        layerSwitch.setClickingTogglesState(true);
        layerSwitch.onClick = [this] { processor.setAVLayerEnabled(layer, layerSwitch.getToggleState()); refresh(); };
        addAndMakeVisible(layerSwitch);

        voice.onClick = [this] { processor.selectAVLayer(layer); refresh(); };
        voice.onContextMenu = [this] { showMenu(); };
        addAndMakeVisible(voice);

        const auto knob = [this](Slider& slider, double low, double high, double interval, double reset, bool bipolar) {
            slider.setSliderStyle(Slider::RotaryHorizontalVerticalDrag);
            slider.setTextBoxStyle(Slider::NoTextBox, false, 0, 0);
            slider.setRange(low, high, interval);
            slider.setDoubleClickReturnValue(true, reset);
            slider.setMouseDragSensitivity(150);
            slider.setWantsKeyboardFocus(true);
            if (bipolar) slider.getProperties().set("AVBipolar", true);
            slider.onValueChange = [this] { store(); repaint(); };
            addAndMakeVisible(slider);
        };
        knob(volume, 0.0, 1.0, 0.0, 1.0, false);
        volume.setSkewFactorFromMidPoint(0.25);
        knob(pan, 0.0, 1.0, 0.0, 0.5, true);
        knob(detune, -DexedAudioProcessor::AVLayerMaxDetune, DexedAudioProcessor::AVLayerMaxDetune, 0.1, 0.0, true);
        knob(transpose, -24.0, 24.0, 1.0, 0.0, true);

        channel.onChange = [this] { store(); };
        addAndMakeVisible(channel);
        for (auto* box : { &lowKey, &highKey }) {
            for (int note = 0; note < 128; ++note) box->addItem(noteName(note), note + 1);
            box->onChange = [this, box] { keysChanged(box == &lowKey); };
            addAndMakeVisible(*box);
        }
        applyLanguage();
        refresh();
    }

    void applyLanguage() {
        auto& language = AVDexedLanguageService::getInstance();
        const auto selected = channel.getSelectedId();
        channel.clear(dontSendNotification);
        channel.addItem(language.text("layerAllChannels"), 1);
        for (int c = 1; c <= 16; ++c) channel.addItem(String(c), c + 1);
        channel.setSelectedId(selected > 0 ? selected : 1, dontSendNotification);
        const auto title = language.format("layerTitle", {{"number", String(layer + 1)}});
        layerSwitch.setTitle(title + ": " + language.text("layerToggleTitle"));
        layerSwitch.setTooltip(language.text("layerToggleTooltip"));
        voice.setTitle(title);
        voice.setTooltip(language.text("layerSelectTooltip"));
        volume.setTitle(title + ": " + language.text("volume"));
        volume.setTooltip(language.text("layerVolumeTooltip"));
        pan.setTitle(title + ": " + language.text("layerPan"));
        pan.setTooltip(language.text("layerPanTooltip"));
        detune.setTitle(title + ": " + language.text("layerDetune"));
        detune.setTooltip(language.text("layerDetuneTooltip"));
        transpose.setTitle(title + ": " + language.text("layerTranspose"));
        transpose.setTooltip(language.text("layerTransposeTooltip"));
        channel.setTitle(title + ": " + language.text("layerChannel"));
        channel.setTooltip(language.text("layerChannelTooltip"));
        lowKey.setTitle(title + ": " + language.text("layerLowKey"));
        highKey.setTitle(title + ": " + language.text("layerHighKey"));
        lowKey.setTooltip(language.text("layerKeysTooltip"));
        highKey.setTooltip(language.text("layerKeysTooltip"));
        refresh();
        repaint();
    }

    /** What the processor holds now (the editor polls; a drag in progress wins). */
    void refresh() {
        const auto& settings = processor.getAVLayerSettings(layer);
        auto& language = AVDexedLanguageService::getInstance();
        const bool on = settings.enabled.load();
        layerSwitch.setToggleState(on, dontSendNotification);
        updating = true;
        const auto follow = [](Slider& slider, double value) {
            if (! slider.isMouseButtonDown() && std::abs(slider.getValue() - value) > 1.0e-6)
                slider.setValue(value, dontSendNotification);
        };
        follow(volume, settings.volume.load());
        follow(pan, settings.pan.load());
        follow(detune, settings.detune.load());
        follow(transpose, settings.transpose.load());
        channel.setSelectedId(settings.channel.load() + 1, dontSendNotification);
        lowKey.setSelectedId(settings.lowNote.load() + 1, dontSendNotification);
        highKey.setSelectedId(settings.highNote.load() + 1, dontSendNotification);
        updating = false;

        const auto name = processor.getAVLayerProgram(layer).getName().trim();
        const int sounding = processor.getAVLayerSoundingVoices(layer);
        const bool selected = processor.getAVSelectedLayer() == layer;
        const int group = settings.alternateGroup.load();
        auto voices = language.format("layerVoices", {{"count", String(sounding)}});
        if (group >= 0)   // TX802 alternate assign: this layer takes turns with others
            voices << String::fromUTF8(" · ") << language.format("layerAlternate", {{"number", String(group + 1)}});
        // DX7II voice functions the layer plays (from a performance).
        if (settings.unison.load() > 0.0f) voices << String::fromUTF8(" · ") << language.text("layerUnison");
        if (settings.randomPitch.load() > 0.0f) voices << String::fromUTF8(" · ") << language.text("layerRandom");
        if (settings.mono.load() == 1) voices << String::fromUTF8(" · ") << language.text("layerMono");
        if (const int table = settings.microTable.load(); table > 0)
            voices << String::fromUTF8(" · ") << language.text("microTuning" + String(table));
        if (name != voice.name || voices != voice.voices || selected != voice.selected || (sounding > 0) != voice.sounding) {
            voice.name = name.isNotEmpty() ? name : String("—");
            voice.voices = voices;
            voice.selected = selected;
            voice.sounding = sounding > 0;
            voice.repaint();
        }
        // An off layer stays editable, dimmed.
        for (auto* child : std::initializer_list<Component*>{ &volume, &pan, &detune, &transpose, &channel, &lowKey, &highKey })
            AVDexedDim::setAlpha(*child, on ? 1.0f : 0.45f);
        if (on != shownOn || selected != shownSelected) {
            shownOn = on;
            shownSelected = selected;
            repaint();
        }
    }

    void paint(Graphics& g) override {
        auto& language = AVDexedLanguageService::getInstance();
        const auto bounds = getLocalBounds().toFloat();
        AVDexedTheme::fillBox(g, bounds, 0.0f);
        if (shownSelected) {   // the layer Play and Advanced edit: an ocean rule on top
            g.setGradientFill(ColourGradient(AVDexedTheme::oceanHighlight, 0.0f, 0.0f, AVDexedTheme::oceanTop, 0.0f, 3.0f, false));
            g.fillRect(bounds.withHeight(3.0f));
        }
        const auto title = language.format("layerTitle", {{"number", String(layer + 1)}});
        const auto titleFont = AVDexedTheme::font(15.0f, "SemiBold");
        const auto titleArea = Rectangle<float>(12.0f, 8.0f, float(layerSwitch.getX()) - 16.0f, 24.0f);
        if (shownSelected) AVDexedTheme::drawOceanText(g, title, titleFont, titleArea, Justification::centredLeft);
        else AVDexedTheme::drawGreyText(g, title, titleFont, titleArea, Justification::centredLeft, shownOn ? 1.0f : 0.6f);

        const auto label = [&g](const String& text, Rectangle<int> area) {
            AVDexedTheme::drawGreyText(g, text, AVDexedTheme::font(11.0f, "Medium"), area.toFloat());
        };
        const auto value = [&g, this](const String& text, Rectangle<int> area) {
            AVDexedTheme::drawGreyText(g, text, AVDexedTheme::font(12.0f, "Medium"), area.toFloat(), Justification::centred,
                                       shownOn ? 1.0f : 0.5f);
        };
        label(language.text("volume"), labelAbove(volume));
        label(language.text("layerPan"), labelAbove(pan));
        label(language.text("layerDetune"), labelAbove(detune));
        label(language.text("layerTranspose"), labelAbove(transpose));
        value(volumeText(volume.getValue()), valueBelow(volume));
        value(panText(pan.getValue()), valueBelow(pan));
        value(signedNumber(detune.getValue(), 1) + " ct", valueBelow(detune));
        value(signedNumber(transpose.getValue(), 0), valueBelow(transpose));
        label(language.text("layerChannel"), channel.getBounds().withY(channel.getY() - 18).withHeight(16));
        label(language.text("layerKeys"), Rectangle<int>(lowKey.getX(), lowKey.getY() - 18, highKey.getRight() - lowKey.getX(), 16));

        // The keys this layer plays, on the 128 MIDI notes.
        const auto& settings = processor.getAVLayerSettings(layer);
        const auto bar = Rectangle<float>(float(lowKey.getX()), float(lowKey.getBottom()) + 10.0f,
                                          float(highKey.getRight() - lowKey.getX()), 6.0f);
        AVDexedTheme::fillBox(g, bar, 3.0f, false, 1.4f);
        const float low = float(settings.lowNote.load()), high = float(settings.highNote.load()) + 1.0f;
        const auto range = bar.withX(bar.getX() + bar.getWidth() * low / 128.0f).withWidth(bar.getWidth() * (high - low) / 128.0f);
        g.setGradientFill(ColourGradient(AVDexedTheme::oceanTop.withMultipliedAlpha(shownOn ? 1.0f : 0.5f), 0.0f, range.getY(),
                                         AVDexedTheme::oceanBottom.withMultipliedAlpha(shownOn ? 1.0f : 0.5f), 0.0f, range.getBottom(), false));
        g.fillRoundedRectangle(range, 3.0f);
    }

    void resized() override {
        const int pad = 12, width = getWidth(), height = getHeight();
        layerSwitch.setBounds(width - pad - 46, 9, 46, 22);
        voice.setBounds(pad, 40, width - 2 * pad, 58);
        // Two rows of knobs, the channel and the keys share the rest evenly.
        const int knobRow = 96, channelRow = 46, keysRow = 64;
        const int free = height - 110 - 10 - (2 * knobRow + channelRow + keysRow);
        const int gap = jmax(8, free / 5);
        int y = 110 + gap;
        const int column = (width - 2 * pad) / 2;
        const int knobSize = jmin(56, column - 8);
        const auto place = [&](Slider& slider, int col) {
            slider.setBounds(pad + col * column + (column - knobSize) / 2, y + 18, knobSize, knobSize);
        };
        place(volume, 0);
        place(pan, 1);
        y += knobRow + gap;
        place(detune, 0);
        place(transpose, 1);
        y += knobRow + gap;
        channel.setBounds(pad, y + 18, width - 2 * pad, 26);
        y += channelRow + gap;
        const int half = (width - 2 * pad - 6) / 2;
        lowKey.setBounds(pad, y + 18, half, 26);
        highKey.setBounds(pad + half + 6, y + 18, width - 2 * pad - half - 6, 26);
    }

private:
    DexedAudioProcessor& processor;
    const int layer;
    LayerSwitch layerSwitch;
    VoiceCard voice;
    Slider volume, pan, detune, transpose;
    ComboBox channel, lowKey, highKey;
    bool updating = false, shownOn = false, shownSelected = false;

    static Rectangle<int> labelAbove(const Slider& slider) {
        return Rectangle<int>(slider.getX() - 20, slider.getY() - 18, slider.getWidth() + 40, 16);
    }
    static Rectangle<int> valueBelow(const Slider& slider) {
        return Rectangle<int>(slider.getX() - 20, slider.getBottom() + 2, slider.getWidth() + 40, 16);
    }

    void store() {
        if (updating) return;
        auto& settings = processor.getAVLayerSettings(layer);
        settings.volume.store(float(volume.getValue()));
        settings.pan.store(float(pan.getValue()));
        settings.detune.store(float(detune.getValue()));
        settings.transpose.store(roundToInt(transpose.getValue()));
        if (channel.getSelectedId() > 0) processor.setAVLayerChannel(layer, channel.getSelectedId() - 1);
    }

    void keysChanged(bool lowChanged) {
        if (updating) return;
        auto& settings = processor.getAVLayerSettings(layer);
        int low = jlimit(0, 127, lowKey.getSelectedId() - 1), high = jlimit(0, 127, highKey.getSelectedId() - 1);
        // The other end follows when they would cross.
        if (low > high) {
            if (lowChanged) high = low;
            else low = high;
        }
        settings.lowNote.store(low);
        settings.highNote.store(high);
        refresh();
        repaint();
    }

    void showMenu() {
        auto& language = AVDexedLanguageService::getInstance();
        PopupMenu menu;
        const int selected = processor.getAVSelectedLayer();
        menu.addItem(1, language.format("layerCopySelected", {{"number", String(selected + 1)}}), selected != layer);
        menu.addItem(2, language.text("layerResetOne"));
        // The DX7II voice functions the layer adds around the engine.
        const auto& settings = processor.getAVLayerSettings(layer);
        PopupMenu unison, random, mono;
        static constexpr float unisonSteps[] = { 0.0f, 3.0f, 5.0f, 7.0f, 12.0f };
        static constexpr float randomSteps[] = { 0.0f, 5.0f, 10.0f, 20.0f, 41.0f };
        const auto amount = [&language](float cents) {
            return cents <= 0.0f ? language.text("optionOff") : String(cents, cents < 10.0f ? 1 : 0) + " ct";
        };
        for (int i = 0; i < 5; ++i) {
            unison.addItem(10 + i, amount(unisonSteps[i]), true, std::abs(settings.unison.load() - unisonSteps[i]) < 0.05f);
            random.addItem(20 + i, amount(randomSteps[i]), true, std::abs(settings.randomPitch.load() - randomSteps[i]) < 0.05f);
        }
        mono.addItem(30, language.text("layerFollowInstrument"), true, settings.mono.load() < 0);
        mono.addItem(31, language.text("layerPoly"), true, settings.mono.load() == 0);
        mono.addItem(32, language.text("layerMonoChoice"), true, settings.mono.load() == 1);
        menu.addSeparator();
        menu.addItem(3, language.text("layerFreePhase"), true, settings.phase.load() >= 0.0f);
        menu.addSubMenu(language.text("layerUnisonMenu"), unison);
        menu.addSubMenu(language.text("layerRandomMenu"), random);
        menu.addSubMenu(language.text("layerMonoMenu"), mono);
        PopupMenu tuning;
        tuning.addItem(40, language.text("optionOff"), true, settings.microTable.load() <= 0);
        for (int table = 1; table <= 10; ++table)
            tuning.addItem(40 + table, language.text("microTuning" + String(table)), true, settings.microTable.load() == table);
        menu.addSubMenu(language.text("layerTuningMenu"), tuning);
        menu.setLookAndFeel(&getLookAndFeel());
        Component::SafePointer<Strip> safeThis(this);
        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&voice), [safeThis, selected](int result) {
            if (safeThis == nullptr) return;
            auto& p = safeThis->processor;
            auto& chosen = p.getAVLayerSettings(safeThis->layer);
            static constexpr float unisonValues[] = { 0.0f, 3.0f, 5.0f, 7.0f, 12.0f };
            static constexpr float randomValues[] = { 0.0f, 5.0f, 10.0f, 20.0f, 41.0f };
            if (result >= 10 && result < 15) chosen.unison.store(unisonValues[result - 10]);
            else if (result >= 20 && result < 25) chosen.randomPitch.store(randomValues[result - 20]);
            else if (result >= 30 && result <= 32) chosen.mono.store(result - 31);
            else if (result >= 40 && result <= 50) p.setAVLayerMicroTuning(safeThis->layer, result == 40 ? -1 : result - 40, 0);
            else if (result == 3) p.setAVLayerFreePhase(safeThis->layer, chosen.phase.load() < 0.0f);
            if (result == 1) {
                p.setAVLayerProgram(safeThis->layer, p.getAVLayerProgram(selected));
                p.setAVLayerEnabled(safeThis->layer, true);
            } else if (result == 2) {
                auto& settings = p.getAVLayerSettings(safeThis->layer);
                p.setAVLayerChannel(safeThis->layer, 0);
                settings.lowNote.store(0);
                settings.highNote.store(127);
                settings.transpose.store(0);
                settings.detune.store(0.0f);
                settings.volume.store(1.0f);
                settings.pan.store(0.5f);
                settings.alternateGroup.store(-1);
                settings.unison.store(0.0f);
                settings.randomPitch.store(0.0f);
                settings.mono.store(-1);
                settings.phase.store(-1.0f);
            }
            safeThis->refresh();
            safeThis->repaint();
        });
    }
};

//==============================================================================
AVDexedLayersView::AVDexedLayersView(DexedAudioProcessor& owner) : processor(owner) {
    modeToggle.setClickingTogglesState(true);
    modeToggle.setToggleState(processor.isAVLayerModeOn(), dontSendNotification);
    modeToggle.onClick = [this] { processor.setAVLayerMode(modeToggle.getToggleState()); refresh(); };
    addChildComponent(modeToggle);   // hidden: the two pills drive it
    addAndMakeVisible(modePills);

    stackButton.onClick = [this] {
        // The classic first; a few more widths and sizes after it.
        auto& language = AVDexedLanguageService::getInstance();
        struct Stack { int layers; float cents; };
        static constexpr Stack stacks[] = { { 8, 7.0f }, { 8, 12.0f }, { 4, 5.0f }, { 2, 3.0f } };
        PopupMenu menu;
        for (int i = 0; i < 4; ++i)
            menu.addItem(i + 1, language.format(i == 0 ? "layerStackClassic" : "layerStackItem",
                                                {{"layers", String(stacks[i].layers)}, {"cents", String(stacks[i].cents, 0)}}));
        menu.setLookAndFeel(&getLookAndFeel());
        Component::SafePointer<AVDexedLayersView> safeThis(this);
        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&stackButton), [safeThis](int result) {
            if (safeThis == nullptr || result < 1 || result > 4) return;
            const auto& chosen = stacks[result - 1];
            safeThis->processor.stackAVLayers(chosen.layers, chosen.cents);
            safeThis->refresh();
            if (safeThis->onNotice)
                safeThis->onNotice(AVDexedLanguageService::getInstance().format("layerStacked", {{"layers", String(chosen.layers)}}));
        });
    };
    resetButton.onClick = [this] {
        processor.resetAVLayers();
        refresh();
        if (onNotice) onNotice(AVDexedLanguageService::getInstance().text("layerResetDone"));
    };
    openButton.onClick = [this] { openPerformanceFile(); };
    saveButton.onClick = [this] { showSaveMenu(); };
    performanceList.onChange = [this] {
        if (performanceList.getSelectedId() > 0) applyPerformance(performanceList.getSelectedId() - 1);
    };
    addChildComponent(performanceList);
    for (auto* button : { &openButton, &saveButton, &stackButton, &resetButton }) addAndMakeVisible(*button);
    for (int layer = 0; layer < DexedAudioProcessor::AVLayerCount; ++layer) {
        strips[size_t(layer)] = std::make_unique<Strip>(processor, layer);
        addAndMakeVisible(*strips[size_t(layer)]);
    }
    applyLanguage();
    refresh();
}

AVDexedLayersView::~AVDexedLayersView() {
    stopTimer();
}

void AVDexedLayersView::visibilityChanged() {
    if (isVisible()) {
        refresh();
        startTimerHz(15);
    } else {
        stopTimer();
    }
}

void AVDexedLayersView::applyLanguage() {
    auto& language = AVDexedLanguageService::getInstance();
    modeToggle.setTitle(language.text("layerMode"));
    modePills.setTexts(language.text("optionOn"), language.text("optionOff"));
    openButton.setButtonText(language.text("layerOpenPerformance"));
    openButton.setTooltip(language.text("layerOpenPerformanceTooltip"));
    saveButton.setButtonText(language.text("layerSave"));
    saveButton.setTooltip(language.text("layerSaveTooltip"));
    performanceList.setTooltip(language.text("layerPerformanceListTooltip"));
    performanceList.setTitle(language.text("layerPerformanceListTooltip"));
    stackButton.setButtonText(language.text("layerStack"));
    stackButton.setTooltip(language.text("layerStackTooltip"));
    resetButton.setButtonText(language.text("layerReset"));
    resetButton.setTooltip(language.text("layerResetTooltip"));
    for (auto& strip : strips) strip->applyLanguage();
    resized();
    repaint();
}

void AVDexedLayersView::refresh() {
    const bool on = processor.isAVLayerModeOn();
    // Notified, so the two pills follow (setAVLayerMode() with the same value does nothing).
    if (modeToggle.getToggleState() != on) modeToggle.setToggleState(on, sendNotificationSync);
    for (auto& strip : strips) {
        strip->setEnabled(on);
        AVDexedDim::setAlpha(*strip, on ? 1.0f : 0.4f);
        strip->refresh();
    }
    stackButton.setEnabled(true);
    resetButton.setEnabled(on);
    if (on != shownMode) {
        shownMode = on;
        repaint();
    }
}

void AVDexedLayersView::timerCallback() {
    refresh();
}

void AVDexedLayersView::paint(Graphics& g) {
    auto& language = AVDexedLanguageService::getInstance();
    AVDexedSettingsStyle::label(g, language.text("layerMode"), modeLabelArea, Justification::centredLeft);
    if (! performanceList.isVisible())
        AVDexedSettingsStyle::hint(g, language.text(shownMode ? "layerHint" : "layerOffHint"), hintArea);
}

bool AVDexedLayersView::isPerformanceOnlyFile(const File& file) {
    MemoryBlock bytes;
    if (! file.getFileExtension().equalsIgnoreCase(".syx") || file.getSize() > 1024 * 1024 || ! file.loadFileAsData(bytes))
        return false;
    const auto contents = AVDexedSysex::scan(bytes);
    return contents.banks.empty() && contents.voices.empty() && ! AVDexedPerformance::readPerformances(contents).empty();
}

void AVDexedLayersView::openPerformanceFile() {
    auto start = performanceFile.getParentDirectory();
    if (! start.isDirectory()) start = File::getSpecialLocation(File::userHomeDirectory).getChildFile("Downloads");
    if (! start.isDirectory()) start = File::getSpecialLocation(File::userDocumentsDirectory);
    chooser = std::make_unique<FileChooser>(AVDexedLanguageService::getInstance().text("layerOpenPerformance"), start,
                                            String("*.syx;*.SYX;") + AVDexedLibrary::presetWildcards + ";*"
                                                + DexedAudioProcessor::AVLegacyLayersExtension, true);
    Component::SafePointer<AVDexedLayersView> safeThis(this);
    chooser->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [safeThis](const FileChooser& result) {
        const auto file = result.getResult();
        if (safeThis == nullptr || ! file.existsAsFile()) return;
        if (file.getFileExtension().equalsIgnoreCase(DexedAudioProcessor::AVLegacyLayersExtension))
            safeThis->loadLayersFile(file);
        else if (AVDexedLibrary::isPresetFile(file)) {
            if (safeThis->onOpenSoundFile) safeThis->onOpenSoundFile(file);
        } else
            safeThis->loadPerformanceFile(file);
    });
}

void AVDexedLayersView::loadLayersFile(const File& file) {
    auto& language = AVDexedLanguageService::getInstance();
    const auto text = file.getSize() <= 4 * 1024 * 1024 ? file.loadFileAsString() : String();
    if (! text.contains("avDexedLayers")) {
        if (onNotice) onNotice(language.format("layerOpenFailed", {{"file", file.getFileName()}}));
        return;
    }
    Component::SafePointer<AVDexedLayersView> safeThis(this);
    const auto open = [safeThis, file, text] {
        if (safeThis != nullptr) safeThis->installLayersFile(file, text);
    };
    if (onReplaceSound) onReplaceSound(open);
    else open();
}

void AVDexedLayersView::installLayersFile(const File& file, const String& text) {
    auto& language = AVDexedLanguageService::getInstance();
    if (! processor.loadAVLayersFile(text)) {
        if (onNotice) onNotice(language.format("layerOpenFailed", {{"file", file.getFileName()}}));
        return;
    }
    showLayerSet(file);
    if (onNotice) onNotice(language.format("layerOpened", {{"file", file.getFileName()}}));
}

void AVDexedLayersView::showLayerSet(const File& file) {
    performanceList.setVisible(false);
    performances.clear();
    voicePool.reset();
    performanceFile = file;
    lastSetName = file.getFileNameWithoutExtension();
    resized();
    refresh();
    repaint();
}

void AVDexedLayersView::showSaveMenu() {
    // As the Save button above: "Guardar" writes the layers' own preset again,
    // "Guardar como" a new one in "User Presets" (the editor does both).
    auto& language = AVDexedLanguageService::getInstance();
    PopupMenu menu;
    menu.addItem(1, language.text("save"));
    menu.addItem(2, language.text("saveAs"));
    menu.setLookAndFeel(&getLookAndFeel());
    Component::SafePointer<AVDexedLayersView> safeThis(this);
    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&saveButton), [safeThis](int result) {
        if (safeThis != nullptr && result > 0 && safeThis->onSaveLayers)
            safeThis->onSaveLayers(result == 2, safeThis->lastSetName);
    });
}

void AVDexedLayersView::loadPerformanceFile(const File& file) {
    auto& language = AVDexedLanguageService::getInstance();
    MemoryBlock bytes;
    if (file.getSize() > 16 * 1024 * 1024 || ! file.loadFileAsData(bytes)) return;
    const auto contents = AVDexedSysex::scan(bytes);
    auto found = AVDexedPerformance::readPerformances(contents);
    if (found.empty()) {
        if (onNotice) onNotice(language.format("layerNoPerformances", {{"file", file.getFileName()}}));
        return;
    }
    performanceFile = file;
    lastSetName = file.getFileNameWithoutExtension();
    performances = std::move(found);
    voicePool = std::make_unique<AVDexedPerformance::VoicePool>(file, contents);
    performanceList.clear(dontSendNotification);
    installedPerformanceId = 0;
    for (size_t i = 0; i < performances.size(); ++i) {
        const auto& performance = performances[i];
        const auto name = performance.name.isNotEmpty() ? performance.name : String::fromUTF8("—");
        performanceList.addItem(String(performance.number).paddedLeft('0', 2) + "  " + name, int(i) + 1);
    }
    performanceList.setVisible(true);
    resized();
    repaint();
    performanceList.setSelectedId(1, sendNotificationSync);
}

void AVDexedLayersView::applyPerformance(int index) {
    if (voicePool == nullptr || ! isPositiveAndBelow(index, int(performances.size()))) return;
    // Until it plays, the list shows what plays: a Cancel in the save-changes
    // question leaves it right, and choosing the same item again works.
    performanceList.setSelectedId(installedPerformanceId, dontSendNotification);
    Component::SafePointer<AVDexedLayersView> safeThis(this);
    const auto install = [safeThis, index] {
        if (safeThis != nullptr) safeThis->installPerformance(index);
    };
    if (onReplaceSound) onReplaceSound(install);
    else install();
}

void AVDexedLayersView::installPerformance(int index) {
    if (voicePool == nullptr || ! isPositiveAndBelow(index, int(performances.size()))) return;
    auto& language = AVDexedLanguageService::getInstance();
    const auto& performance = performances[size_t(index)];
    const auto layers = AVDexedPerformance::resolve(performance, *voicePool);
    processor.applyAVLayerSetup(layers);
    installedPerformanceId = index + 1;
    performanceList.setSelectedId(installedPerformanceId, dontSendNotification);
    if (performance.name.isNotEmpty()) lastSetName = performance.name;
    int used = 0, missing = 0;
    for (const auto& layer : layers) {
        used += layer.plan.used ? 1 : 0;
        missing += layer.plan.used && ! layer.voiceFound ? 1 : 0;
    }
    auto notice = language.format("layerPerformanceLoaded", {{"name", performance.name}, {"count", String(used)},
                                                             {"sources", voicePool->sources().joinIntoString(", ")}});
    if (missing > 0) notice << language.format("layerVoicesMissing", {{"count", String(missing)}});
    // What the original does that does not play here.
    if (performance.notes.contains("pan")) notice << language.text("layerNotePan");
    if (performance.notes.contains("microTuning")) notice << language.text("layerNoteMicro");
    refresh();
    if (onNotice) onNotice(notice);
}

void AVDexedLayersView::resized() {
    const int toolbar = 40;
    const auto& language = AVDexedLanguageService::getInstance();
    const float labelWidth = GlyphArrangement::getStringWidth(AVDexedSettingsStyle::body(), language.text("layerMode")) + 16.0f;
    modeLabelArea = { 4.0f, 0.0f, labelWidth, float(toolbar) };
    modePills.setBounds(int(modeLabelArea.getRight()), (toolbar - 30) / 2, modePills.idealWidth(), 30);
    const int resetWidth = AVDexedSettingsStyle::pillWidth(resetButton.getButtonText(), 90);
    const int stackWidth = AVDexedSettingsStyle::pillWidth(stackButton.getButtonText(), 120);
    const int openWidth = AVDexedSettingsStyle::pillWidth(openButton.getButtonText(), 90);
    const int saveWidth = AVDexedSettingsStyle::pillWidth(saveButton.getButtonText(), 90);
    resetButton.setBounds(getWidth() - resetWidth, (toolbar - 30) / 2, resetWidth, 30);
    stackButton.setBounds(resetButton.getX() - 10 - stackWidth, (toolbar - 30) / 2, stackWidth, 30);
    saveButton.setBounds(stackButton.getX() - 10 - saveWidth, (toolbar - 30) / 2, saveWidth, 30);
    openButton.setBounds(saveButton.getX() - 10 - openWidth, (toolbar - 30) / 2, openWidth, 30);
    hintArea = { float(modePills.getRight() + 24), 0.0f, float(openButton.getX() - 24 - modePills.getRight() - 24), float(toolbar) };
    performanceList.setBounds(hintArea.toNearestInt().withSizeKeepingCentre(jmin(380, int(hintArea.getWidth())), 30)
                                  .withX(int(hintArea.getX())));
    const int gap = 10, top = toolbar + 12;
    const int count = DexedAudioProcessor::AVLayerCount;
    const int width = (getWidth() - gap * (count - 1)) / count;
    for (int layer = 0; layer < count; ++layer) {
        const int x = layer * (width + gap);
        const int w = layer == count - 1 ? getWidth() - x : width;
        strips[size_t(layer)]->setBounds(x, top, w, getHeight() - top);
    }
}
