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

#include "PluginEditor.h"
#include "window/ParamDialog.h"
#include "window/AVDexedSettingsPanel.h"
#include "AVDexedStandaloneAccess.h"
#include "midi/SysexComm.h"
#include "midi/MidiCCHandler.h"
#include "window/TuningShow.h"
#include "../debugger/Logger.h"
#include "math.h"
#include <fstream>
#include <memory>

#include "../msfa/fm_op_kernel.h"

namespace {
juce::String readNativePresetId(const juce::File& file) {
    juce::MemoryBlock data;
    if (!file.loadFileAsData(data))
        return {};
    if (auto xml = juce::AudioProcessor::getXmlFromBinary(data.getData(), static_cast<int>(data.getSize())))
        return juce::ValueTree::fromXml(*xml).getProperty("libraryPresetId").toString();
    return {};
}

juce::MemoryBlock createNativePresetState(DexedAudioProcessor& processor, const juce::String& presetId,
                                         const juce::String& name = {}) {
    return processor.createAVNativePresetState(name.isEmpty() ? processor.getAVPresetName() : name, presetId);
}
}

AVDexedPanicButton::AVDexedPanicButton() {
    setButtonText({});
    setLabel("Panic");
    if (auto svg = XmlDocument::parse(String::fromUTF8(BinaryData::AVDexed_PanicIcon_svg,
                                                        BinaryData::AVDexed_PanicIcon_svgSize)))
        icon = Drawable::createFromSVG(*svg);
}

void AVDexedPanicButton::setLabel(const String& text) {
    label = text;
    setTitle(text);
    repaint();
}

void AVDexedPanicButton::paintButton(Graphics& g, bool hover, bool down) {
    const auto bounds = getLocalBounds().toFloat();
    const auto top = hover || down ? AVDexedTheme::dangerTop : AVDexedTheme::textPrimary;
    const auto bottom = hover || down ? AVDexedTheme::dangerBottom : AVDexedTheme::textSecondary;
    const auto iconBounds = Rectangle<float>(18.0f, 18.0f).withCentre({bounds.getX() + 13.0f, bounds.getCentreY()});
    if (icon != nullptr) {
        auto path = icon->getOutlineAsPath();
        path.applyTransform(path.getTransformToScaleToFit(iconBounds, true));
        g.setGradientFill(ColourGradient(top, 0, iconBounds.getY(), bottom, 0, iconBounds.getBottom(), false));
        g.fillPath(path);
    }
    const auto labelFont = AVDexedTheme::font(10.0f, "Medium");
    g.setFont(labelFont);
    AVDexedTheme::textFill(g, labelFont, top, bottom, 4.0f, float(getHeight() - 4));
    g.drawText(label, getLocalBounds().withTrimmedLeft(26).withTrimmedRight(2), Justification::centred);
    if (AVDexedTheme::showsFocus(*this)) {
        g.setColour(AVDexedTheme::oceanHighlight);
        g.drawRoundedRectangle(bounds.reduced(1), 4, 1.0f);
    }
}

//==============================================================================
DexedAudioProcessorEditor::DexedAudioProcessorEditor (DexedAudioProcessor* ownerFilter)
    : AudioProcessorEditor (ownerFilter),
      midiKeyboard (ownerFilter->keyboardState),
      playView (*ownerFilter),
      cartManager(this), global(*ownerFilter, this) {
    processor = ownerFilter;
    auto& language = AVDexedLanguageService::getInstance();
    language.configure(processor->dexedAppDir);
    language.addChangeListener(this);

    setExplicitFocusOrder(1);

    frameComponent.setBounds(0, 0, WINDOW_SIZE_X, WINDOW_SIZE_Y);
    addAndMakeVisible(&frameComponent);
    setLookAndFeel(&*lookAndFeel);
    background = AVDexedTheme::canvas;

    brandLabel.setText("AV Dexed", dontSendNotification);
    brandLabel.setFont(Font(FontOptions().withHeight(21.0f).withStyle("Bold")));
    brandLabel.setColour(Label::textColourId, AVDexedTheme::oceanHighlight);
    brandLabel.setBounds(20, 7, 145, 32);
    frameComponent.addAndMakeVisible(brandLabel);
    identityLabel.setText("AV Vibes", dontSendNotification);
    identityLabel.setFont(Font(FontOptions().withHeight(10.0f).withStyle("Medium")));
    identityLabel.setColour(Label::textColourId, AVDexedTheme::textSecondary);
    identityLabel.setBounds(22, 38, 130, 16);
    frameComponent.addAndMakeVisible(identityLabel);

    for (auto* button : std::array<Button*, 3>{ &playTabButton, &advancedTabButton, &saveButton }) {
        button->setColour(TextButton::buttonColourId, AVDexedTheme::panelTop);
        button->setColour(TextButton::buttonOnColourId, AVDexedTheme::oceanBottom);
        button->setColour(TextButton::textColourOffId, AVDexedTheme::textSecondary);
        button->setColour(TextButton::textColourOnId, AVDexedTheme::textPrimary);
        frameComponent.addAndMakeVisible(*button);
    }
    panicButton.setColour(TextButton::buttonColourId, AVDexedTheme::panelTop);
    panicButton.setColour(TextButton::buttonOnColourId, AVDexedTheme::oceanBottom);
    panicButton.setColour(TextButton::textColourOffId, AVDexedTheme::textSecondary);
    panicButton.setColour(TextButton::textColourOnId, AVDexedTheme::textPrimary);
    frameComponent.addAndMakeVisible(panicButton);
    saveButton.setColour(TextButton::buttonColourId, AVDexedTheme::oceanBottom);
    saveButton.setColour(TextButton::buttonOnColourId, AVDexedTheme::oceanTop);
    panicButton.setColour(TextButton::buttonColourId, AVDexedTheme::dangerBottom);
    panicButton.setColour(TextButton::buttonOnColourId, AVDexedTheme::dangerTop);
    playTabButton.onClick = [this] { showAVDexedPlayView(true); };
    playTabButton.getProperties().set("AVTab", true);
    advancedTabButton.getProperties().set("AVTab", true);
    // GOZO tabs switch on mouse down.
    playTabButton.setTriggeredOnMouseDown(true);
    advancedTabButton.setTriggeredOnMouseDown(true);
    advancedTabButton.onClick = [this] { showAVDexedPlayView(false); };
    saveButton.onClick = [this] {
        auto& language = AVDexedLanguageService::getInstance();
        PopupMenu menu;
        menu.addItem(1, language.text("save"));
        menu.addItem(2, language.text("saveAs"));
        Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
        menu.setLookAndFeel(&getLookAndFeel());
        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&saveButton), [safeThis](int result) {
            if (safeThis != nullptr && result > 0)
                safeThis->saveNativePreset(result == 2);
        });
    };
    if (auto svg = XmlDocument::parse(String::fromUTF8(BinaryData::AVDexed_SaveIcon_svg, BinaryData::AVDexed_SaveIcon_svgSize))) {
        auto icon = Drawable::createFromSVG(*svg);
        auto blueSvg = XmlDocument::parse(String::fromUTF8(BinaryData::AVDexed_SaveIcon_svg, BinaryData::AVDexed_SaveIcon_svgSize)
            .replace("#E0E0E0", "#" + AVDexedTheme::oceanTop.toDisplayString(false)).replace("#9A9A9A", "#" + AVDexedTheme::oceanBottom.toDisplayString(false)));
        auto hover = blueSvg ? Drawable::createFromSVG(*blueSvg) : nullptr;
        saveButton.setImages(icon.get(), hover.get(), hover.get());
        saveButton.setEdgeIndent(2);
    }
    languageButton.onClick = [&language] {
        language.setLanguage(language.isSpanish() ? "en" : "es");
    };
    panicButton.onClick = [this] { midiKeyboard.releaseAVUINotes(); processor->requestAVPanic(); };
    playTabButton.setBounds(180, 10, 78, 38);
    advancedTabButton.setBounds(262, 10, 104, 38);
    panicButton.setBounds(874, 10, 80, 38);
    languageButton.setBounds(960, 10, 52, 38);
    saveButton.setBounds(1020, 10, 80, 38);

    activePresetLabel.setFont(Font(16.0f, Font::bold));
    activePresetLabel.setColour(Label::textColourId, AVDexedTheme::textPrimary);
    activePresetLabel.setMinimumHorizontalScale(0.8f);
    activePresetLabel.setBounds(390, 10, 390, 38);
    frameComponent.addChildComponent(activePresetLabel);
    modifiedLabel.setFont(Font(11.0f, Font::bold));
    modifiedLabel.setColour(Label::textColourId, AVDexedTheme::oceanHighlight);
    modifiedLabel.setJustificationType(Justification::centredRight);
    modifiedLabel.setBounds(782, 10, 98, 38);
    frameComponent.addChildComponent(modifiedLabel);

    playView.onSaveRequested = [this] { saveNativePreset(false); };
    playView.onSaveAsRequested = [this] { saveNativePreset(true); };
    playView.onPresetLoadRequested = [this](const File& file) {
        loadNativePreset(file);
    };
    playView.onProgramSelectionRequested = [this](int program, bool fromStock) {
        requestPresetReplacement([this, program, fromStock] {
            playView.commitProgramSelection(program, fromStock);
            rebuildProgramCombobox();
            refreshPresetHeader();
        });
    };
    playView.onImportBankRequested = [this] {
        fileChooser = std::make_unique<FileChooser>(AVDexedLanguageService::getInstance().text("importBankTitle"), processor->dexedCartDir,
                                                    "*.syx;*.SYX", true);
        fileChooser->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                 [this](const FileChooser& chooser) {
            if (!chooser.getResults().isEmpty())
                loadCart(chooser.getResult());
        });
    };
    frameComponent.addAndMakeVisible(playView);
    frameComponent.addAndMakeVisible(playView.getPresetNavigator());
    playView.onOperatorSelected = [this](int op) {
        showAVDexedPlayView(false);
        if (isPositiveAndBelow(op, 6) && operators[op] != nullptr) {
            operators[op]->grabKeyboardFocus();
            operators[op]->flashHighlight();
        }
    };
    playView.setBounds(24, 70, WINDOW_SIZE_X - 48, 550);
    frameComponent.addAndMakeVisible(classicView);
    classicView.setBounds(24, 70, WINDOW_SIZE_X - 48, 750);

    // OPERATORS
    for (int i=0;i<6;i++) {
        operators[i] = std::make_unique<OperatorEditor>(*processor, i);
        classicView.addAndMakeVisible(operators[i].get());
    }
    // One shared keyboard/state across both views.
    frameComponent.addAndMakeVisible(midiKeyboard);
    midiKeyboard.setScrollButtonsVisible(false);
    midiKeyboard.setWantsKeyboardFocus(true);
    midiKeyboard.setKeyPressBaseOctave(2);
    for (auto* wheel : {&AVPitchWheel, &AVModWheel}) {
        frameComponent.addAndMakeVisible(*wheel);
        wheel->setSliderStyle(Slider::LinearVertical);
        wheel->setTextBoxStyle(Slider::NoTextBox, false, 0, 0);
        wheel->setWantsKeyboardFocus(true);
    }
    AVPitchWheel.getProperties().set("AVPitch", true);
    AVPitchWheel.setRange(0, 16383, 1);
    AVPitchWheel.setValue(8192, dontSendNotification);
    AVPitchWheel.setDoubleClickReturnValue(true, 8192);
    AVPitchWheel.onValueChange = [this] { processor->setAVKeyboardPitch(int(AVPitchWheel.getValue())); };
    AVModWheel.setRange(0, 127, 1);
    AVModWheel.setDoubleClickReturnValue(true, 0);
    AVModWheel.onValueChange = [this] { processor->setAVKeyboardMod(int(AVModWheel.getValue())); };
    for (auto* label : {&AVPitchLabel, &AVModLabel, &AVOctaveLabel}) {
        label->setFont(AVDexedTheme::font(11));
        label->setJustificationType(Justification::centred);
        label->setColour(Label::textColourId, AVDexedTheme::textSecondary);
        frameComponent.addAndMakeVisible(*label);
    }
    for (auto* label : {&AVHelpLabel, &AVCpuLabel, &AVVoicesLabel}) {
        label->setFont(AVDexedTheme::font(14.0f));
        label->setColour(Label::textColourId, AVDexedTheme::textSecondary);
        frameComponent.addAndMakeVisible(*label);
    }
    if (auto svg = XmlDocument::parse(String::fromUTF8(BinaryData::AVDexed_InfoIcon_svg, BinaryData::AVDexed_InfoIcon_svgSize)))
        AVInfoSymbol = Drawable::createFromSVG(*svg);
    frameComponent.addAndMakeVisible(AVSettingsButton);
    frameComponent.addAndMakeVisible(AVOutputMeter);
    AVSettingsButton.onClick = [this] { parmShow(); };
    if (auto xml = XmlDocument::parse(String::fromUTF8(BinaryData::AVDexed_SettingsIcon_svg, BinaryData::AVDexed_SettingsIcon_svgSize))) {
        auto icon = Drawable::createFromSVG(*xml);
        auto blueSvg = XmlDocument::parse(String::fromUTF8(BinaryData::AVDexed_SettingsIcon_svg, BinaryData::AVDexed_SettingsIcon_svgSize)
            .replace("#E0E0E0", "#" + AVDexedTheme::oceanTop.toDisplayString(false)).replace("#9A9A9A", "#" + AVDexedTheme::oceanBottom.toDisplayString(false)));
        auto hover = blueSvg ? Drawable::createFromSVG(*blueSvg) : nullptr;
        AVSettingsButton.setImages(icon.get(), hover.get(), hover.get());
        AVSettingsButton.setEdgeIndent(2);
    }
    if (auto svg = XmlDocument::parse(String::fromUTF8(BinaryData::AVDexed_Logo_svg, BinaryData::AVDexed_Logo_svgSize)))
        AVBrandSymbol = Drawable::createFromSVG(*svg);

    frameComponent.addAndMakeVisible(resizeGrip);

    // The global component
    classicView.addAndMakeVisible(&global);
    global.setBounds(0, 566, WINDOW_SIZE_X - 48, 184);
    global.cartButton->onClick = [this]() { cartShow(); };
    global.parmButton->onClick = [this]() { parmShow(); };
    global.initButton->onClick = [this]() { initProgram(); };
    global.storeButton->onClick = [this]() { storeProgram(); };

    resizeGrip.onResizeStart = [this] { resizeFactorAtStart = processor->getZoomFactor(); };
    resizeGrip.onResizeDrag = [this](Point<int> screenDelta) {
        const auto logicalHeight = processor->showKeyboard ? WINDOW_SIZE_Y : WINDOW_SIZE_Y - 112;
        const float width = static_cast<float>(WINDOW_SIZE_X);
        const float height = static_cast<float>(logicalHeight);
        const float dx = static_cast<float>(screenDelta.x);
        const float dy = static_cast<float>(screenDelta.y);
        const float step = (width * dx + height * dy) / (width * width + height * height);
        processor->setZoomFactor(jlimit(0.80f, 1.6f, resizeFactorAtStart + step));
        resetSize();
    };
    resizeGrip.onResizeEnd = [this] { processor->savePreference(); };

    rebuildProgramCombobox();
    global.programs->addListener(this);

    classicView.addChildComponent(&cartManagerCover);
    cartManagerCover.addChildComponent(&cartManager);
    cartManager.setVisible(true);

    AffineTransform scale = AffineTransform::scale(processor->getZoomFactor());
    frameComponent.setTransform(scale);
    setResizable(true, false);
    if (auto* display = Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
        const auto fit = jmin(float(display->userArea.getWidth() - 32) / WINDOW_SIZE_X,
                              float(display->userArea.getHeight() - 64) / WINDOW_SIZE_Y);
        processor->setZoomFactor(jlimit(0.80f, 1.6f, jmin(processor->getZoomFactor(), fit)));
    }
    resetSize();
    showAVDexedPlayView(true, false);
    applyLanguage();
    addKeyListener(this);
    addMouseListener(&focusModality, true);
    if (JUCEApplicationBase::isStandaloneApp())
        AVDexedStandaloneAccess::AVShowProductSettings = [this] { parmShow(); };
    updateUI();
    startTimer(20);
}

DexedAudioProcessorEditor::~DexedAudioProcessorEditor() {
    if (JUCEApplicationBase::isStandaloneApp())
        AVDexedStandaloneAccess::AVShowProductSettings = {};
    PopupMenu::dismissAllActiveMenus();  // their LookAndFeel belongs to this editor
    removeMouseListener(&focusModality);
    stopTimer();
    processor->midiCCMapper->cancelMapping();
    if (AVSettingsOverlay) Desktop::getInstance().getAnimator().cancelAnimation(AVSettingsOverlay.get(), false);
    midiKeyboard.releaseAVUINotes();
    Desktop::getInstance().getAnimator().cancelAnimation(&classicView, false);
    Desktop::getInstance().getAnimator().cancelAnimation(&playView, false);
    AVDexedLanguageService::getInstance().removeChangeListener(this);
    setLookAndFeel(nullptr);
}

//==============================================================================
void DexedAudioProcessorEditor::paint (Graphics& g) {
    g.fillAll(background);
    Graphics::ScopedSaveState save(g);
    g.addTransform(AffineTransform::scale(float(getWidth()) / WINDOW_SIZE_X));
    const auto keyboardArea = Rectangle<float>(0.0f, float(frameComponent.getHeight() - 158), WINDOW_SIZE_X, 128.0f);
    if (processor->showKeyboard && g.clipRegionIntersects(keyboardArea.toNearestInt())) {
        // Keyboard zone: panel gradient under a 50% black veil, like GOZO.
        const auto area = keyboardArea;
        AVDexedTheme::panel(g, area, 0.0f);
        g.setColour(Colours::black.withAlpha(0.5f));
        g.fillRect(area);
    }
    const float footer = float(frameComponent.getHeight() - 30);
    g.setColour(AVDexedTheme::canvas);
    g.fillRect(Rectangle<float>(0, footer, WINDOW_SIZE_X, 30));
    if (AVInfoSymbol) AVInfoSymbol->drawWithin(g, {21, footer + 7.5f, 15, 15}, RectanglePlacement::centred, 1);
    // Brand block: logo, "AV Vibes" and a short rule, as in GOZO's top bar.
    if (!g.clipRegionIntersects({0, 0, 600, 88}))
        return;
    if (AVBrandSymbol)
        AVBrandSymbol->drawWithin(g, Rectangle<float>(30, AVTopBarAxis - 22.0f, 186, 44), RectanglePlacement::xLeft | RectanglePlacement::yMid, 1.0f);
    const auto identity = AVDexedTheme::font(13.0f, "SemiBold");
    AVDexedTheme::drawGreyText(g, "AV Vibes", identity, {33, AVTopBarAxis + 22.0f, 80, 18}, Justification::centredLeft);
    const float ruleX = 33.0f + GlyphArrangement::getStringWidth(identity, "AV Vibes") + 8.0f;
    AVDexedTheme::fillBox(g, {ruleX, AVTopBarAxis + 30.0f, 56.0f, 2.0f}, 0.0f, false, 3.0f);
    // Separator between the tabs.
    const auto separator = Rectangle<float>(float(playTabButton.getRight()) + 10.0f, AVTopBarAxis - 13.0f, 2.0f, 26.0f);
    AVDexedTheme::fillBox(g, separator, 0.0f, false, 4.0f);
}

void DexedAudioProcessorEditor::showAVDexedPlayView(bool showPlay, bool animate) {
    midiKeyboard.releaseAVUINotes();
    if (cartManagerCover.isVisible()) cartManager.hideCartridgeManager();
    PopupMenu::dismissAllActiveMenus();
    auto& outgoing = showPlay ? classicView : static_cast<Component&>(playView);
    auto& incoming = showPlay ? static_cast<Component&>(playView) : classicView;
    const bool isChangingView = playTabButton.getToggleState() != showPlay;
    const auto generation = ++viewTransitionGeneration;
    auto& animator = Desktop::getInstance().getAnimator();
    animator.cancelAnimation(&outgoing, false);
    animator.cancelAnimation(&incoming, false);
    incoming.setInterceptsMouseClicks(true, true);
    if (animate && isChangingView) {
        outgoing.setInterceptsMouseClicks(false, false);
        incoming.setAlpha(0.0f);
        incoming.setVisible(true);
        // GOZO tab change: a 220 ms linear cross-fade.
        animator.fadeOut(&outgoing, 220);
        animator.fadeIn(&incoming, 220);
        Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
        Timer::callAfterDelay(230, [safeThis, generation, showPlay] {
            if (safeThis == nullptr || safeThis->viewTransitionGeneration != generation)
                return;
            auto& oldView = showPlay ? safeThis->classicView : static_cast<Component&>(safeThis->playView);
            oldView.setVisible(false);
            oldView.setAlpha(1.0f);
            oldView.setInterceptsMouseClicks(true, true);
        });
    } else {
        outgoing.setVisible(false);
        outgoing.setAlpha(1.0f);
        outgoing.setInterceptsMouseClicks(true, true);
        incoming.setAlpha(1.0f);
        incoming.setVisible(true);
    }
    playTabButton.setToggleState(showPlay, dontSendNotification);
    advancedTabButton.setToggleState(!showPlay, dontSendNotification);
    if (showPlay)
        playView.refreshLabels();
}

void DexedAudioProcessorEditor::applyLanguage() {
    auto& language = AVDexedLanguageService::getInstance();
    playTabButton.setButtonText(language.text("play"));
    advancedTabButton.setButtonText(language.text("advanced"));
    saveButton.setButtonText(language.text("save"));
    saveButton.setTitle(language.text("save"));
    saveButton.setTooltip(language.text("save"));
    panicButton.setLabel({});
    panicButton.setTitle(language.text("panic"));
    panicButton.setTooltip(language.text("panic"));
    languageButton.setButtonText(language.getLanguage().toUpperCase());
    languageButton.setTooltip(language.text("changeLanguage"));
    brandLabel.setText("AV Dexed", dontSendNotification);
    identityLabel.setText("AV Vibes", dontSendNotification);
    AVPitchLabel.setText(language.text("keyboardPitchShort"), dontSendNotification);
    AVModLabel.setText(language.text("keyboardModShort"), dontSendNotification);
    AVPitchWheel.setTitle(language.text("keyboardPitch"));
    AVModWheel.setTitle(language.text("keyboardMod"));
    midiKeyboard.setTitle(language.text("keyboard"));
    AVSettingsButton.setTitle(language.text("settingsTitle"));
    AVSettingsButton.setTooltip(language.text("settingsTitle"));
    AVUpdateKeyboardRange();
    advancedTabButton.setTooltip(language.text("advancedTooltip"));
    resizeGrip.setTitle(language.text("resizeEditor"));
    resizeGrip.setTooltip(language.text("resizeTooltip"));
    global.applyLanguage();
    for (auto& op : operators)
        if (op != nullptr)
            op->applyLanguage();
    playView.applyLanguage();
    refreshPresetHeader();
}

void DexedAudioProcessorEditor::changeListenerCallback(ChangeBroadcaster*) {
    applyLanguage();
}

void DexedAudioProcessorEditor::refreshPresetHeader() {
    activePresetLabel.setText(processor->getAVPresetName(), dontSendNotification);
    playView.refreshLabels();
    modifiedLabel.setText(playView.isModified()
                              ? AVDexedLanguageService::getInstance().text("modified")
                              : String(), dontSendNotification);
}

void DexedAudioProcessorEditor::requestPresetReplacement(std::function<void()> replacement) {
    if (!replacement)
        return;

    playView.refreshLabels();
    if (!playView.isModified()) {
        replacement();
        return;
    }

    auto& language = AVDexedLanguageService::getInstance();
    auto* dialog = new AVAlertWindow(language.text("saveChangesTitle"),
                                   language.text("saveChanges"), AlertWindow::QuestionIcon, this);
    dialog->addButton(language.text("save"), 1, KeyPress(KeyPress::returnKey));
    dialog->addButton(language.text("discard"), 2, KeyPress());
    dialog->addButton(language.text("cancel"), 0, KeyPress(KeyPress::escapeKey));
    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    dialog->enterModalState(true, ModalCallbackFunction::create(
        [safeThis, replacement = std::move(replacement)](int response) mutable {
            if (safeThis == nullptr)
                return;
            if (response == 1) {
                safeThis->saveNativePreset(false, [safeThis, replacement = std::move(replacement)](bool saved) mutable {
                    if (safeThis == nullptr)
                        return;
                    if (saved)
                        replacement();
                    else
                        safeThis->updateUI();
                });
            } else if (response == 2) {
                replacement();
            } else {
                safeThis->updateUI();
            }
        }), true);
}

void DexedAudioProcessorEditor::cartShow() {
    stopTimer();
    cartManager.resetActiveSysex();
    cartManagerCover.setBounds(classicView.getLocalBounds());
    cartManager.setBounds(16, 16, cartManagerCover.getWidth() - 32, cartManagerCover.getHeight() - 32);
    cartManager.updateCartFilename();
    cartManagerCover.setVisible(true);
    cartManager.initialFocus();
}

std::unique_ptr<ComponentTraverser> DexedAudioProcessorEditor::createFocusTraverser() {
    return std::make_unique<FocusTraverser>();
}

void DexedAudioProcessorEditor::loadCart(File file) {
    auto& language = AVDexedLanguageService::getInstance();
    Cartridge cart;

    int rc = cart.load(file);

    if ( rc < 0 ) {
        AVShowMessage(AlertWindow::WarningIcon, language.text("presetError"),
                      language.format("bankOpenError", {{"path", file.getFullPathName()}}), this);
        return;
    }

    if ( rc != 0 ) {
        AlertWindow::showOkCancelBox(AlertWindow::QuestionIcon, language.text("invalidBankTitle"),
                                     language.text("invalidBank") + " " + language.text("invalidBankQuestion"),
                                     language.text("loadRandom"), language.text("cancel"), this,
                                     ModalCallbackFunction::create([this, cart, file](int result) {
                                         if ( result != 0 )
                                             applyCart(cart, file);
                                     }));
        return;
    }

    applyCart(cart, file);
}

void DexedAudioProcessorEditor::applyCart(Cartridge cart, File file) {
    requestPresetReplacement([this, cart = std::move(cart), file]() mutable {
        commitCart(std::move(cart), file);
    });
}

void DexedAudioProcessorEditor::commitCart(Cartridge cart, const File& file) {
    processor->loadCartridge(cart);
    rebuildProgramCombobox();
    processor->setCurrentProgram(0);
    global.programs->setSelectedId(processor->getCurrentProgram()+1, dontSendNotification);
    processor->updateHostDisplay();

    processor->activeFileCartridge = file;
    playView.setActiveUserPreset({});
    playView.updateBank();
    refreshPresetHeader();
}

void DexedAudioProcessorEditor::saveCart() {
    fileChooser = std::make_unique<FileChooser>(AVDexedLanguageService::getInstance().text("exportBankTitle"), processor->dexedCartDir, "*.syx;*.SYX", true);
    fileChooser->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                             [this](const FileChooser& fc) {
                                 if ( fc.getResults().isEmpty() )
                                     return;
                                 Cartridge snapshot(processor->currentCart);
                                 snapshot.packProgram(processor->activeProgram.getRawData(), processor->getCurrentProgram(),
                                                      processor->activeProgram.getName());
                                 if (snapshot.saveVoice(fc.getResult()) <= 0) {
                                     AVShowMessage(AlertWindow::WarningIcon, AVDexedLanguageService::getInstance().text("saveError"),
                                                   AVDexedLanguageService::getInstance().format("writeError", {{"path", fc.getResult().getFullPathName()}}), this);
                                 }
                             });
}

void DexedAudioProcessorEditor::tuningShow() {
    auto te = new TuningShow();
    te->setTuning( processor->synthTuningState->getTuning() );

    DialogWindow::LaunchOptions options;
    options.content.setOwned(te);
    options.dialogTitle = AVDexedLanguageService::getInstance().text("tuningTitle");
    options.dialogBackgroundColour = AVDexedTheme::panelTop;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;

    auto dialogwindow = options.launchAsync();
}

namespace {
class AVDexedSettingsOverlay final : public Component {
    Image AVBlurredBackdrop;
    std::unique_ptr<AVDexedSettingsPanel> AVPanel;
    std::function<void()> AVClose;
public:
    AVDexedSettingsOverlay(Image backdrop, std::unique_ptr<AVDexedSettingsPanel> panel, std::function<void()> close)
        : AVPanel(std::move(panel)), AVClose(std::move(close)) {
        // A transient in-memory blur of vector-rendered content, never a PNG UI asset.
        const auto small = backdrop.rescaled(backdrop.getWidth() / 3, backdrop.getHeight() / 3);
        AVBlurredBackdrop = small.createCopy();
        ImageConvolutionKernel kernel(9);
        kernel.createGaussianBlur(2.0f);
        kernel.applyToImage(AVBlurredBackdrop, small, small.getBounds());
        addAndMakeVisible(*AVPanel);
        setWantsKeyboardFocus(true);
        setFocusContainerType(FocusContainerType::focusContainer);
    }
    void paint(Graphics& g) override {
        // The footer stays uncovered: it carries the help text, as in GOZO.
        g.reduceClipRegion(0, 0, getWidth(), getHeight() - 30);
        g.drawImage(AVBlurredBackdrop, getLocalBounds().toFloat());
        g.setColour(Colours::black.withAlpha(0.54f));
        g.fillAll();
    }
    bool hitTest(int, int y) override { return y < getHeight() - 30; }
    void resized() override {
        AVPanel->setBounds(Rectangle<int>(1180, 650).withCentre(getLocalBounds().withTrimmedBottom(30).getCentre()));
    }
    bool keyPressed(const KeyPress& key) override { return AVPanel->keyPressed(key); }
    void mouseDown(const MouseEvent& event) override {
        if (!AVPanel->getBounds().contains(event.getPosition()) && AVClose) AVClose();
    }
    void inputAttemptWhenModal() override { if (AVClose) AVClose(); }
};
}

void DexedAudioProcessorEditor::parmShow() {
    midiKeyboard.releaseAVUINotes();
    if (AVSettingsOverlay && AVSettingsOverlay->isVisible()) {
        AVSettingsOverlay->toFront(true);
        return;
    }
    AVSettingsOverlay.reset();
    auto backdrop = createComponentSnapshot(getLocalBounds()).rescaled(frameComponent.getWidth(), frameComponent.getHeight());
    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    AVDexedSettingsPanel::AVCallbacks actions;
    actions.openAudioSettings = AVDexedStandaloneAccess::AVOpenAudioSettings;
    actions.close = [safeThis] {
        if (!safeThis || !safeThis->AVSettingsOverlay) return;
        // Hide immediately; destroy after the button callback has unwound.
        Desktop::getInstance().getAnimator().cancelAnimation(safeThis->AVSettingsOverlay.get(), false);
        safeThis->AVSettingsOverlay->exitModalState(0);
        // GOZO settings timing: in ~6 frames, out ~8 frames.
        Desktop::getInstance().getAnimator().fadeOut(safeThis->AVSettingsOverlay.get(), 133);
        Timer::callAfterDelay(1, [safeThis] {
            if (safeThis && safeThis->AVSettingsOverlay && !safeThis->AVSettingsOverlay->isVisible())
                safeThis->AVSettingsOverlay.reset();
        });
    };
    actions.applyInstrumentSettings = [safeThis](ParamDialog* dialog) {
        if (!safeThis) return;
        float scale = safeThis->processor->getZoomFactor();
        const bool opened = dialog->getDialogValues(safeThis->processor->controllers, safeThis->processor->sysexComm,
                                                    &safeThis->processor->showKeyboard, &scale);
        safeThis->processor->setZoomFactor(scale);
        safeThis->processor->savePreference();
        safeThis->resetSize();
        if (!opened) {
            auto& language = AVDexedLanguageService::getInstance();
            auto* alert = new AVAlertWindow(language.text("midiInterface"), language.text("midiPortsError"), AlertWindow::WarningIcon);
            alert->addButton(language.text("ok"), 1, KeyPress(KeyPress::returnKey), KeyPress(KeyPress::escapeKey));
            alert->enterModalState(true, nullptr, true);
        }
    };
    actions.tuningAction = [safeThis](ParamDialog* dialog, ParamDialog::TuningAction action) {
        if (!safeThis) return;
        switch (action) {
            case ParamDialog::LOAD_SCL: safeThis->processor->applySCLTuning(); break;
            case ParamDialog::LOAD_KBM: safeThis->processor->applyKBMMapping(); break;
            case ParamDialog::RESET_TUNING: safeThis->processor->retuneToStandard(); break;
            case ParamDialog::SHOW_TUNING: safeThis->tuningShow(); break;
        }
        dialog->setIsStandardTuning(safeThis->processor->synthTuningState->is_standard_tuning());
    };
    const auto closeOverlay = actions.close;
    AVSettingsOverlay = std::make_unique<AVDexedSettingsOverlay>(std::move(backdrop),
        std::make_unique<AVDexedSettingsPanel>(*processor, std::move(actions)), closeOverlay);
    // Added hidden: ComponentAnimator::fadeIn() does nothing to a component that
    // is already visible and opaque, which left only the fade-out working.
    frameComponent.addChildComponent(*AVSettingsOverlay);
    AVSettingsOverlay->setBounds(frameComponent.getLocalBounds());
    AVSettingsOverlay->enterModalState(true);
    Desktop::getInstance().getAnimator().fadeIn(AVSettingsOverlay.get(), 100);
}

void DexedAudioProcessorEditor::initProgram() {
    requestPresetReplacement([this] {
        processor->resetToInitVoice();
        playView.setActiveUserPreset({});
        playView.captureCurrentAsBaseline();
        processor->updateHostDisplay();
        refreshPresetHeader();
    });
}

void DexedAudioProcessorEditor::comboBoxChanged (ComboBox* comboBoxThatHasChanged) {
    if (comboBoxThatHasChanged != global.programs)
        return;
    const int selectedProgram = global.programs->getSelectedId() - 1;
    requestPresetReplacement([this, selectedProgram] {
        processor->setCurrentProgram(selectedProgram);
        processor->updateHostDisplay();
        playView.setActiveUserPreset({});
        playView.captureCurrentAsBaseline();
        updateUI();
    });
}

void DexedAudioProcessorEditor::timerCallback() {
    const auto now = Time::getMillisecondCounterHiRes();
    const auto peaks = processor->consumeAVOutputPeaks();
    AVOutputMeter.AVUpdate(peaks.first, peaks.second, AVLastMeterUpdate > 0.0 ? float((now - AVLastMeterUpdate) / 1000.0) : 0.02f);
    AVLastMeterUpdate = now;
    AVUpdateFooter();
    if (!AVPitchWheel.isBeingDragged()) AVPitchWheel.setValue(processor->getAVKeyboardPitch(), dontSendNotification);
    if (!AVModWheel.isBeingDragged()) AVModWheel.setValue(processor->getAVKeyboardMod(), dontSendNotification);
    if (resizeSaveCountdown > 0) {
        resizeSaveCountdown -= 20;
        if (resizeSaveCountdown <= 0)
            processor->savePreference();
    }

    if ( processor->programChanged.compareAndSetBool(false, true) ) {
        updateUI();
    }

    if (++labelRefreshTicks >= 5) {
        labelRefreshTicks = 0;
        refreshPresetHeader();
    }

    if ( ! processor->peekVoiceStatus() ) {
        // Nothing sounds: let the Play glow fade and zero the operator meters once.
        playView.setOperatorLevels({});
        if (!AVOperatorsSilent) {
            AVOperatorsSilent = true;
            for (auto& op : operators) op->updateGain(0.0f);
        }
        if (!approximatelyEqual(AVLastGlobalVu, AVOutputMeter.AVDisplayedGain())) {
            AVLastGlobalVu = AVOutputMeter.AVDisplayedGain();
            global.updateVu(AVLastGlobalVu);
        }
        return;
    }
    AVOperatorsSilent = false;

    std::array<float, 6> heroLevels {};
    for(int i=0;i<6;i++) {
        //operators[i].updateGain(sqrt(processor->voiceStatus.amp[5 - i]) / 8196);        // TODO: FUGLY !!!! change this sqrt nonsense

        const int amp_min = 1036152;
        const float one_per_amp_diff = (float)(1.0 / (259037922 - amp_min));
        // Note, that minimum and maximum values of ``amp`` are different and depend on the engines. 
        // These two constants were determined from the results produced by the OPL engine,
        // because its minimum is smaller, its maximum is higher than in the other cases. 

        int amp = processor->voiceStatus.amp[5 - i] - amp_min;
        if (amp <= 0) amp = 0;
        operators[i]->updateGain(amp * one_per_amp_diff);
        operators[i]->updateEnvPos(processor->voiceStatus.ampStep[5 - i]);
        // Play's glow reads the envelope level on a 48 dB log scale topped at
        // the loudest possible operator (L = 4064), so modulators with modest
        // output levels still light instead of staying dark.
        const auto raw = double(jmax<uint32_t>(1, processor->voiceStatus.amp[5 - i]));
        heroLevels[size_t(i)] = jlimit(0.0f, 1.0f, float(1.0 + (std::log2(raw) - 25.875) / 8.0));
    }
    playView.setOperatorLevels(heroLevels);
    global.updatePitchPos(processor->voiceStatus.pitchStep);
    AVLastGlobalVu = AVOutputMeter.AVDisplayedGain();
    global.updateVu(AVLastGlobalVu);
}

void DexedAudioProcessorEditor::updateUI() {
    for(int i=0;i<6;i++) {
        operators[i]->updateDisplay();
    }
    rebuildProgramCombobox();
    global.updateDisplay();
    cartManager.updateCartFilename();
    playView.updateBank();
    refreshPresetHeader();
}

void DexedAudioProcessorEditor::rebuildProgramCombobox() {
    global.programs->clear(dontSendNotification);

    processor->currentCart.getProgramNames(processor->programNames);

    for(int i=0;i<processor->getNumPrograms();i++) {
        String id;
        id << (i+1) << ". " << processor->getProgramName(i);
        global.programs->addItem(id, i+1);
    }

    global.programs->setSelectedId(processor->getCurrentProgram()+1, dontSendNotification);

    String name = processor->activeProgram.getName();
    cartManager.setActiveProgram(processor->getCurrentProgram(), name);
    if ( name != processor->getProgramName(processor->getCurrentProgram()) )
        global.programs->setText("**. " + name, dontSendNotification);

    cartManager.resetActiveSysex();
}

void DexedAudioProcessorEditor::storeProgram() {
    storeProgramShowDialog(processor->currentCart, File());
}

void DexedAudioProcessorEditor::saveNativePreset(bool saveAs, std::function<void(bool)> completion) {
    const auto activeFile = playView.getActiveUserPreset();
    if (!saveAs && activeFile.existsAsFile()) {
        const auto oldKey = playView.getPresetKeyForFile(activeFile);
        auto presetId = readNativePresetId(activeFile);
        if (presetId.isEmpty())
            presetId = Uuid().toString();
        auto state = createNativePresetState(*processor, presetId);
        if (state.isEmpty()) {
            if (completion) completion(false);
            return;
        }
        if (!activeFile.replaceWithData(state.getData(), state.getSize())) {
            AVShowMessage(AlertWindow::WarningIcon, AVDexedLanguageService::getInstance().text("saveError"),
                          AVDexedLanguageService::getInstance().format("writeError", {{"path", activeFile.getFullPathName()}}), this);
            if (completion) completion(false);
            return;
        }
        playView.migratePresetKey(oldKey, "user:" + presetId);
        processor->setAVPresetContext(processor->getAVPresetName(), presetId, activeFile);
        processor->captureAVPresetBaseline();
        playView.setActiveUserPreset(activeFile);
        processor->updateHostDisplay();
        if (completion) completion(true);
        return;
    }

    auto folder = processor->dexedAppDir.getChildFile("User Presets");
    folder.createDirectory();
    auto fileName = File::createLegalFileName(processor->getAVPresetName().trim());
    if (fileName.isEmpty()) fileName = "Untitled";
    fileName << ".dexedpreset";
    const auto initialFile = activeFile.existsAsFile() && saveAs
        ? activeFile : folder.getChildFile(fileName);
    fileChooser = std::make_unique<FileChooser>(AVDexedLanguageService::getInstance().text("saveNativeTitle"), initialFile, "*.dexedpreset", true);
    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    fileChooser->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles
                             | FileBrowserComponent::warnAboutOverwriting,
                             [safeThis, saveAs, activeFile, completion = std::move(completion)](const FileChooser& chooser) mutable {
        if (safeThis == nullptr)
            return;
        if (chooser.getResults().isEmpty()) {
            if (completion) completion(false);
            return;
        }
        auto destination = chooser.getResult();
        if (destination.getFileExtension().compareIgnoreCase(".dexedpreset") != 0)
            destination = destination.withFileExtension("dexedpreset");
        destination.getParentDirectory().createDirectory();
        auto presetId = !saveAs && destination == activeFile ? readNativePresetId(destination) : String();
        const auto oldKey = !saveAs && destination == activeFile
            ? safeThis->playView.getPresetKeyForFile(destination) : String();
        if (presetId.isEmpty())
            presetId = Uuid().toString();
        auto state = createNativePresetState(*safeThis->processor, presetId, destination.getFileNameWithoutExtension());
        if (state.isEmpty()) {
            if (completion) completion(false);
            return;
        }
        if (!destination.replaceWithData(state.getData(), state.getSize())) {
            AVShowMessage(AlertWindow::WarningIcon, AVDexedLanguageService::getInstance().text("saveError"),
                          AVDexedLanguageService::getInstance().format("writeError", {{"path", destination.getFullPathName()}}), safeThis.getComponent());
            if (completion) completion(false);
            return;
        }
        safeThis->playView.migratePresetKey(oldKey, "user:" + presetId);
        safeThis->processor->setAVPresetContext(destination.getFileNameWithoutExtension(), presetId, destination);
        safeThis->processor->captureAVPresetBaseline();
        safeThis->playView.setActiveUserPreset(destination);
        safeThis->processor->updateHostDisplay();
        safeThis->updateUI();
        if (completion) completion(true);
    });
}

void DexedAudioProcessorEditor::loadNativePreset(const File& file) {
    auto& language = AVDexedLanguageService::getInstance();
    if (!file.existsAsFile() || file.getSize() <= 0 || file.getSize() > 32 * 1024 * 1024) {
        AVShowMessage(AlertWindow::WarningIcon, language.text("presetError"),
                      language.text("missingPreset"), this);
        return;
    }
    MemoryBlock state;
    if (!file.loadFileAsData(state)) {
        AVShowMessage(AlertWindow::WarningIcon, language.text("presetError"),
                      language.format("readFileError", {{"path", file.getFullPathName()}}), this);
        return;
    }
    auto xml = AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    if (xml == nullptr) {
        AVShowMessage(AlertWindow::WarningIcon, language.text("presetError"),
                      language.text("invalidPreset"), this);
        return;
    }
    const bool legacyState = xml->hasTagName("dexedState");
    const bool currentRoot = xml->hasTagName(processor->parameters.rootVt.getType().toString());
    bool structurallyValid = legacyState;
    if (currentRoot) {
        const auto tree = ValueTree::fromXml(*xml);
        const auto version = tree.getProperty(IDs::version).toString();
        const int schema = static_cast<int>(tree.getProperty("schemaVersion", 0));
        const auto parameterTree = tree.getChildWithName(IDs::parameters);
        structurallyValid = (version == DexedApvts::MODEL_VERSION || version == "1.0.0")
            && schema >= 0 && schema <= 2
            && parameterTree.isValid() && parameterTree.getType() == IDs::parameters;

        const auto session = tree.getChildWithName("session");
        if (structurallyValid && session.isValid()) {
            MemoryBlock voice;
            const auto encodedVoice = session.getProperty("voice").toString();
            const int program = static_cast<int>(session.getProperty("currentProgram", -1));
            structurallyValid = voice.fromBase64Encoding(encodedVoice)
                && voice.getSize() == Program::PROGRAM_SIZE + 1
                && program >= 0 && program < processor->getNumPrograms();
            const auto encodedCart = session.getProperty("cartridge").toString();
            if (structurallyValid && encodedCart.isNotEmpty()) {
                MemoryBlock cartData;
                Cartridge decodedCart;
                structurallyValid = cartData.fromBase64Encoding(encodedCart)
                    && cartData.getSize() == SYSEX_SIZE
                    && decodedCart.load(static_cast<const uint8_t*>(cartData.getData()),
                                        static_cast<int>(cartData.getSize())) >= 0
                    && decodedCart.containsData();
            }
        }
    }
    if (!structurallyValid) {
        AVShowMessage(AlertWindow::WarningIcon, language.text("presetError"),
                      language.text("invalidPreset"), this);
        return;
    }

    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    requestPresetReplacement([safeThis, file, state = std::move(state)]() mutable {
        if (safeThis == nullptr)
            return;
        if (!safeThis->processor->loadAVStateInformation(state.getData(), static_cast<int>(state.getSize()))) {
            auto& language = AVDexedLanguageService::getInstance();
            AVShowMessage(AlertWindow::WarningIcon, language.text("presetError"),
                          language.text("invalidPreset"), safeThis.getComponent());
            return;
        }
        safeThis->processor->setAVPresetContext(safeThis->processor->getAVPresetName(), readNativePresetId(file), file);
        safeThis->processor->captureAVPresetBaseline();
        safeThis->playView.setActiveUserPreset(file);
        safeThis->processor->updateHostDisplay();
        safeThis->updateUI();
    });
}

void DexedAudioProcessorEditor::storeProgramShowDialog(Cartridge destSysex, File externalFile) {
    auto& language = AVDexedLanguageService::getInstance();
    String currentName = processor->activeProgram.getName();
    bool activeCartridgeFound = processor->activeFileCartridge.exists();
    bool hasExternalFile = externalFile.exists();
    String msg;

    if ( ! hasExternalFile ) {
        if ( activeCartridgeFound )
            msg = language.text("storeProgramCurrentNewNamed")
                .replace("$bank", processor->activeFileCartridge.getFileName());
        else
            msg = language.text("storeProgramCurrentNew");
    } else {
        msg = language.text("storeProgramTo").replace("$bank", externalFile.getFileName());
    }

    auto dialog = new AVAlertWindow(language.text("storeProgramTitle"), msg, AlertWindow::NoIcon, this);
    dialog->addTextEditor("Name", currentName, language.text("nameLimit"), false);
    String asciiNameCharacters;
    for (juce_wchar character = 32; character <= 126; ++character)
        asciiNameCharacters += character;
    dialog->getTextEditor("Name")->setInputRestrictions(10, asciiNameCharacters);

    StringArray programs;
    destSysex.getProgramNames(programs);
    dialog->addComboBox("Dest", programs, language.text("programDestination"));

    if ( ! hasExternalFile ) {
        StringArray saveAction;
        saveAction.add(language.text("storeProgramDaw"));
        saveAction.add(language.text("storeProgramNewBank"));
        if ( activeCartridgeFound )
            saveAction.add(language.text("storeProgramOverwrite"));

        dialog->addComboBox("SaveAction", saveAction, language.text("storeAction"));
    }

    dialog->addButton(language.text("store"), 0, KeyPress(KeyPress::returnKey));
    dialog->addButton(language.text("cancel"), 1, KeyPress(KeyPress::escapeKey));
    dialog->addButton(language.text("externalFile"), 2, KeyPress());

    dialog->enterModalState(true, ModalCallbackFunction::create(
        [this, dialog, destSysex, externalFile, hasExternalFile](int response) {
            storeProgramResponse(dialog, response, destSysex, externalFile, hasExternalFile);
        }), true);
}

void DexedAudioProcessorEditor::storeProgramResponse(AlertWindow *dialog, int response, Cartridge destSysex, File externalFile, bool hasExternalFile) {
    if ( response == 2 ) {
        fileChooser = std::make_unique<FileChooser>(AVDexedLanguageService::getInstance().text("destinationSysex"),
                                                    processor->dexedCartDir, "*.syx;*.SYX;*.*", true);
        fileChooser->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                 [this, destSysex](const FileChooser& fc) mutable {
                                     if ( fc.getResults().isEmpty() ) {
                                         cartManager.resetActiveSysex();
                                         return;
                                     }
                                     File chosenFile = fc.getResult();
                                     if ( destSysex.load(chosenFile) == 0 ) {
                                         storeProgramShowDialog(destSysex, chosenFile);
                                         return;
                                     }
                                     AVShowMessage(AlertWindow::WarningIcon, AVDexedLanguageService::getInstance().text("presetError"),
                                                   AVDexedLanguageService::getInstance().text("readError"), this);
                                     cartManager.resetActiveSysex();
                                 });
        return;
    }

    if ( response == 0 ) {
        TextEditor *name = dialog->getTextEditor("Name");
        ComboBox *dest = dialog->getComboBoxComponent("Dest");

        int programNum = dest->getSelectedItemIndex();
        String programName(name->getText());
        Program storedVoice = processor->activeProgram;
        storedVoice.setName(programName);

        if (!destSysex.packProgram(storedVoice.getRawData(), programNum, programName)) {
            const auto& language = AVDexedLanguageService::getInstance();
            AVShowMessage(AlertWindow::WarningIcon, language.text("saveError"),
                          language.text("voicePackError"), this);
            cartManager.resetActiveSysex();
            return;
        }

        auto adoptStoredBank = [this, programNum, programName](const Cartridge& savedCart, const File& source) {
            playView.carryAVBankMetadata(processor->currentCart, savedCart);
            processor->loadCartridge(savedCart);
            processor->activeFileCartridge = source;
            processor->activeProgram.setName(programName);
        processor->setAVPresetContext({}, {}, {});
        processor->captureAVPresetBaseline();
            processor->setCurrentProgramSelection(programNum);
            processor->updateHostDisplay();
            rebuildProgramCombobox();
            cartManager.resetActiveSysex();
        };

        if ( ! hasExternalFile ) {
            int action = dialog->getComboBoxComponent("SaveAction")->getSelectedItemIndex();
            if (action == 0) {
                adoptStoredBank(destSysex, processor->activeFileCartridge);
                return;
            }

            if (action == 1) {
                fileChooser = std::make_unique<FileChooser>(AVDexedLanguageService::getInstance().text("destinationSysex"),
                                                            processor->dexedCartDir, "*.syx;*.SYX", true);
                fileChooser->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                                         [this, destSysex, programNum, programName, adoptStoredBank](const FileChooser& fc) mutable {
                                             if (fc.getResults().isEmpty()) {
                                                 cartManager.resetActiveSysex();
                                                 return;
                                             }

                                             File destination = fc.getResult();
                                             if (destSysex.saveVoice(destination) <= 0) {
                                                 const auto& language = AVDexedLanguageService::getInstance();
                                                 AVShowMessage(AlertWindow::WarningIcon, language.text("saveError"),
                                                               language.format("writeError", {{"path", destination.getFullPathName()}}), this);
                                                 cartManager.resetActiveSysex();
                                                 return;
                                             }
                                             adoptStoredBank(destSysex, destination);
                                         });
                return;
            }

            File destination = processor->activeFileCartridge;
            if (!destination.existsAsFile() || destSysex.saveVoice(destination) <= 0) {
                const auto& language = AVDexedLanguageService::getInstance();
                AVShowMessage(AlertWindow::WarningIcon, language.text("saveError"),
                              language.format("writeError", {{"path", destination.getFullPathName()}}), this);
                cartManager.resetActiveSysex();
                return;
            }
            adoptStoredBank(destSysex, destination);
            return;
        } else {
            if (destSysex.saveVoice(externalFile) <= 0) {
                const auto& language = AVDexedLanguageService::getInstance();
                AVShowMessage(AlertWindow::WarningIcon, language.text("saveError"),
                              language.format("writeError", {{"path", externalFile.getFullPathName()}}), this);
                cartManager.resetActiveSysex();
                return;
            }

            if (externalFile == processor->activeFileCartridge)
                adoptStoredBank(destSysex, externalFile);
        }
    }

    cartManager.resetActiveSysex();
}

float DexedAudioProcessorEditor::getLargestScaleFactor() {
    // constexpr float TESTING_SCALE_FACTOR[] = { 4.0f, 3.0f, 2.0f, 1.5f, 1.0f };
    //
    // for (float factor: TESTING_SCALE_FACTOR) {
    //     const juce::Rectangle<int> rect(WINDOW_SIZE_X * factor, WINDOW_SIZE_Y * factor);
    //
    //     // validate if there is really a display that can show the complete plugin size
    //     for (auto& display : Desktop::getInstance().getDisplays().displays) {
    //         int height = display.userArea.getHeight();
    //         int width = display.userArea.getWidth();
    //
    //         TRACE("Testing size %d x %d < Dexed Window %d x %d", height, width, rect.getWidth(), rect.getHeight() );
    //         if ( height > rect.getHeight() && width > rect.getWidth() ) {
    //             TRACE("Found factor %f for display %s with size %d x %d", factor, display.userArea.toString().toRawUTF8(), height, width );
    //             return factor;
    //         }
    //     }
    // }
    //
    // TRACE("No suitable display found, returning default scale factor 1.0");

    // For now, always return 4.0f as the maximum scale factor.
    return 4.0f;
}

void DexedAudioProcessorEditor::resetZoomFactor() {
    TRACE("Resetting zoom factor to 1.0");
    processor->setZoomFactor(1.0);
    processor->savePreference();
    AffineTransform scale = AffineTransform::scale(processor->getZoomFactor());
    frameComponent.setTransform(scale);
    resetSize();
}

bool DexedAudioProcessorEditor::isInterestedInFileDrag (const StringArray &files) {
    if( files.size() != 1 ) return false;

    for( auto i = files.begin(); i != files.end(); ++i ) {
        if( i->endsWithIgnoreCase( ".scl" ) || i->endsWithIgnoreCase( ".kbm" )
            || i->endsWithIgnoreCase(".syx") || i->endsWithIgnoreCase(".dexedpreset"))
            return true;
    }
    return false;
}

void DexedAudioProcessorEditor::filesDropped (const StringArray &files, int x, int y ) {
    if( files.size() != 1 ) return;
    auto fn = files[0];
    if (fn.endsWithIgnoreCase(".dexedpreset")) {
        loadNativePreset(File(fn));
        return;
    }
    if (fn.endsWithIgnoreCase(".syx")) {
        loadCart(File(fn));
        return;
    }
    const bool scale = fn.endsWithIgnoreCase(".scl");
    if (!scale && !fn.endsWithIgnoreCase(".kbm"))
        return;
    auto& language = AVDexedLanguageService::getInstance();
    const File file(fn);
    String problem;
    if (!file.existsAsFile())
        problem = language.format("fileAccessError", {{"path", fn}});
    else if (file.getSize() <= 0)
        problem = language.text("tuningFileEmpty");
    else if (file.getSize() > MAX_SCL_KBM_FILE_SIZE)
        problem = language.format("tuningFileSize", {{"bytes", String(MAX_SCL_KBM_FILE_SIZE)}});
    if (problem.isNotEmpty()) {
        AVShowMessage(AlertWindow::WarningIcon, language.text("tuningFileError"), problem, this);
        return;
    }
    if (scale)
        processor->applySCLTuning(file);
    else
        processor->applyKBMMapping(file);
}

void DexedAudioProcessorEditor::AVFocusModality::mouseDown(const MouseEvent& event) {
    if (!AVDexedTheme::keyboardFocusVisible())
        return;
    AVDexedTheme::keyboardFocusVisible() = false;
    if (auto* top = event.eventComponent->getTopLevelComponent())
        top->repaint();
}

bool DexedAudioProcessorEditor::keyPressed(const KeyPress& key, Component* originatingComponent) {
    if (AVDexedTheme::noteKeyboardNavigation(key))
        repaint();
    if (dynamic_cast<TextEditor*>(Component::getCurrentlyFocusedComponent()) != nullptr)
        return false;
    int keycode = key.getKeyCode();
    ModifierKeys mods = key.getModifiers();

    #ifdef DEXED_EVENT_DEBUG
        TRACE("key pressed: %d\n", keycode);
    #endif

    if ( (keycode >= '1' && keycode <= '6') && mods.isCtrlDown() ) {
        int op = keycode - '1';

        if ( mods.isShiftDown() ) {
            operators[op]->toggleOpSwitch();
            return true;
        }

        operators[op]->grabKeyboardFocus();
        return true;
    }

    if ( keycode == 'G' && mods.isCtrlDown() ) {
        global.grabKeyboardFocus();
        return true;
    }

    if ( keycode == 'L' && mods.isCtrlDown() ) {
        cartShow();
        return true;
    }

    if ( keycode == 'P' && mods.isCtrlDown() ) {
        parmShow();
        return true;
    }

    if ( key.getKeyCode() == KeyPress::escapeKey ) {
        cartManager.hideCartridgeManager();
        return true;
    }

    return false;
}

void DexedAudioProcessorEditor::AVUpdateFooter() {
    auto& language = AVDexedLanguageService::getInstance();
    String help;
    auto* hover = Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
    if (hover && (hover == this || isParentOf(hover))) {
        for (auto* component = hover; component && component != this; component = component->getParentComponent()) {
            if (auto* tooltip = dynamic_cast<TooltipClient*>(component)) help = tooltip->getTooltip();
            if (help.isEmpty()) help = component->getTitle();
            if (help.isNotEmpty()) break;
        }
    }
    // Knobs and faders also show their current value, e.g. "Reverb · Amount: 32%".
    for (auto* component = hover; component != nullptr && component != this && help.isNotEmpty();
         component = component->getParentComponent())
        if (auto* slider = dynamic_cast<Slider*>(component)) {
            help = help.upToFirstOccurrenceOf("\n", false, false) + ": " + slider->getTextFromValue(slider->getValue());
            break;
        }
    AVHelpLabel.setText(help.isEmpty() ? language.text("footerHelp") : help, dontSendNotification);
    // CPU and voices read four times a second, like a status bar, not per frame.
    if (++AVStatusTicks >= 12) {
        AVStatusTicks = 0;
        AVCpuLabel.setText(String::fromUTF8("CPU · ") + String(processor->getAVAudioCpuPercent(), 1) + "%", dontSendNotification);
        AVVoicesLabel.setText(language.text("voices") + ": " + String(processor->getAVActiveVoices()) + " / 16", dontSendNotification);
    }
}

void DexedAudioProcessorEditor::AVUpdateKeyboardRange() {
    const int octaves = jlimit(1, 6, (midiKeyboard.getWidth() / 32 - 1) / 7);
    const int highest = jmin(120, AVKeyboardAnchor + 12 * octaves);
    const int whiteKeys = ((highest - AVKeyboardAnchor) / 12) * 7 + 1;
    midiKeyboard.setAvailableRange(AVKeyboardAnchor, highest);
    midiKeyboard.setKeyWidth(float(jmax(1, midiKeyboard.getWidth())) / float(whiteKeys));
    midiKeyboard.setLowestVisibleKey(AVKeyboardAnchor);
    AVOctaveLabel.setText("C" + String(AVKeyboardAnchor / 12 - 1) + String::fromUTF8(" – C") + String(highest / 12 - 1), dontSendNotification);

}

void DexedAudioProcessorEditor::AVLayoutFrame() {
    constexpr int topBar = 88;
    const int height = frameComponent.getHeight();
    brandLabel.setVisible(false);
    identityLabel.setVisible(false);
    languageButton.setVisible(false);
    activePresetLabel.setVisible(false);
    modifiedLabel.setVisible(false);
    // Top bar: tabs after the brand, preset navigation, then save · meter · settings.
    const auto tabFont = AVDexedTheme::font(30.0f, "SemiBold");
    const int playWidth = roundToInt(GlyphArrangement::getStringWidth(tabFont, playTabButton.getButtonText())) + 12;
    const int advancedWidth = roundToInt(GlyphArrangement::getStringWidth(tabFont, advancedTabButton.getButtonText())) + 12;
    // Everything in the top bar shares one centre line (AVTopBarAxis): the logo
    // mark, the tab text, the preset name with its arrows and star, and the
    // icons, which keep GOZO's even 14 px gaps against a 30 px right margin.
    const int axis = int(AVTopBarAxis);
    playTabButton.setBounds(262, axis - 21, playWidth, 42);
    advancedTabButton.setBounds(playTabButton.getRight() + 22, axis - 21, advancedWidth, 42);
    AVSettingsButton.setBounds(WINDOW_SIZE_X - 60, axis - 15, 30, 30);
    AVOutputMeter.setBounds(AVSettingsButton.getX() - 14 - 16, axis - 15, 16, 30);
    saveButton.setBounds(AVOutputMeter.getX() - 14 - 30, axis - 15, 30, 30);
    const int navLeft = advancedTabButton.getRight() + 50;
    playView.getPresetNavigator().setBounds(navLeft, axis - 21, saveButton.getX() - 50 - navLeft, 58);
    const int bottom = processor->showKeyboard ? 158 : 30;
    const int contentBottom = height - bottom;
    playView.setBounds(0, topBar, WINDOW_SIZE_X, contentBottom - topBar);
    const auto content = Rectangle<int>(20, topBar + 2, WINDOW_SIZE_X - 40, contentBottom - topBar - 10);
    classicView.setBounds(content);
    const int globalHeight = 178;
    const int gap = 10;
    const int rowHeight = (content.getHeight() - globalHeight - 2 * gap) / 2;
    const int columnWidth = (content.getWidth() - gap * 2) / 3;
    for (int i = 0; i < 6; ++i) {
        const int col = i % 3, row = i / 3;
        const int width = col == 2 ? content.getWidth() - col * (columnWidth + gap) : columnWidth;
        operators[i]->setBounds(col * (columnWidth + gap), row * (rowHeight + gap), width, rowHeight);
    }
    global.setBounds(0, content.getHeight() - globalHeight, content.getWidth(), globalHeight);
    const int ky = height - 158;
    for (auto* child : std::array<Component*, 6>{&midiKeyboard, &AVPitchWheel, &AVModWheel,
                       &AVPitchLabel, &AVModLabel, &AVOctaveLabel})
        child->setVisible(processor->showKeyboard);
    AVPitchLabel.setBounds(20, ky + 8, 48, 18);
    AVModLabel.setBounds(71, ky + 8, 48, 18);
    AVPitchWheel.setBounds(26, ky + 28, 36, 70);
    AVModWheel.setBounds(77, ky + 28, 36, 70);
    AVOctaveLabel.setBounds(16, ky + 102, 114, 16);
    midiKeyboard.setBounds(148, ky + 12, WINDOW_SIZE_X - 172, 104);
    AVUpdateKeyboardRange();
    resizeGrip.setBounds(WINDOW_SIZE_X - 30, height - 30, 30, 30);
    AVHelpLabel.setBounds(45, height - 30, WINDOW_SIZE_X - 480, 30);
    AVCpuLabel.setBounds(WINDOW_SIZE_X - 340, height - 30, 120, 30);
    AVVoicesLabel.setBounds(WINDOW_SIZE_X - 214, height - 30, 120, 30);
    panicButton.setBounds(WINDOW_SIZE_X - 76, height - 27, 26, 24);
    AVUpdateFooter();
    if (AVSettingsOverlay) AVSettingsOverlay->setBounds(frameComponent.getLocalBounds());
    if (cartManagerCover.isVisible()) {
        cartManagerCover.setBounds(classicView.getLocalBounds());
        cartManager.setBounds(cartManagerCover.getLocalBounds().reduced(16));
    }
}

void DexedAudioProcessorEditor::resetSize() {
    const float factor = jlimit(0.80f, 1.6f, processor->getZoomFactor());
    processor->setZoomFactor(factor);
    const int logicalHeight = processor->showKeyboard ? WINDOW_SIZE_Y : WINDOW_SIZE_Y - 112;
    frameComponent.setBounds(0, 0, WINDOW_SIZE_X, logicalHeight);
    frameComponent.setTransform(AffineTransform::scale(factor));
    AVLayoutFrame();
    setResizeLimits(roundToInt(WINDOW_SIZE_X * 0.80f), roundToInt(logicalHeight * 0.80f),
                    roundToInt(WINDOW_SIZE_X * 1.6f), roundToInt(logicalHeight * 1.6f));
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio(double(WINDOW_SIZE_X) / logicalHeight);
    const int width = roundToInt(WINDOW_SIZE_X * factor);
    setSize(width, roundToInt(logicalHeight * (float(width) / WINDOW_SIZE_X)));
}

void DexedAudioProcessorEditor::resized() {
    if (processor == nullptr || getWidth() <= 0) return;
    const float factor = jlimit(0.80f, 1.6f, float(getWidth()) / WINDOW_SIZE_X);
    frameComponent.setTransform(AffineTransform::scale(factor));
    if (!approximatelyEqual(factor, processor->getZoomFactor())) {
        processor->setZoomFactor(factor);
        resizeSaveCountdown = 500;
    }
    repaint();
}
