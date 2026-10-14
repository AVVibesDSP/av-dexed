// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedPlayView.h"
#include "AVDexedKnob.h"
#include "../parameter/Model.h"
#include "../JuceLibraryCode/BinaryData.h"
#include "util/ContextMenuAdapter.h"
#include <algorithm>
#include <memory>

using namespace juce;

namespace {
enum class AVEffectKind { reverb, delay, chorus };
ChangeBroadcaster avBrowserPreferenceChanges;

class AVQuickMidiContextMenu final : public ContextMenuAdapter {
public:
    explicit AVQuickMidiContextMenu(DexedAudioProcessor& owner) : ContextMenuAdapter(owner) {}
    void mouseDown(const MouseEvent& event) override {
        if (!event.mods.isPopupMenu()) return;
        auto* target = event.eventComponent;
        // Preserve the ordinary text-edit context menu while a value is typed.
        if (dynamic_cast<TextEditor*>(target) != nullptr) return;
        while (target != nullptr && dynamic_cast<Slider*>(target) == nullptr)
            target = target->getParentComponent();
        if (target != nullptr)
            ContextMenuAdapter::mouseDown(event.getEventRelativeTo(target));
    }
};

std::unique_ptr<Drawable> avDrawableFromSvg(const char* data, int size) {
    if (auto svg = XmlDocument::parse(String::fromUTF8(data, size)))
        return Drawable::createFromSVG(*svg);
    return {};
}

// Drawable::replaceColour only touches solid fills, so gradient icons are
// recoloured in the SVG text: ocean stops become the grey text gradient.
std::unique_ptr<Drawable> avGreyDrawableFromSvg(const char* data, int size) {
    const auto text = String::fromUTF8(data, size)
        .replace("#58BFE8", "#E0E0E0", true).replace("#0B3558", "#9A9A9A", true)
        .replace("#248FC7", "#E0E0E0", true);
    if (auto svg = XmlDocument::parse(text))
        return Drawable::createFromSVG(*svg);
    return {};
}

String avStableStateHash(const void* data, size_t size) {
    auto* bytes = static_cast<const uint8*>(data);
    uint64 hash = 1469598103934665603ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return String::toHexString(static_cast<int64>(hash));
}

class AVEffectSettingsPanel final : public Component, private ChangeListener {
    struct Row {
        Label label;
        Slider slider;
        ComboBox choice;
        ToggleButton toggle;
        std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
        std::unique_ptr<AudioProcessorValueTreeState::ComboBoxAttachment> choiceAttachment;
        std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> buttonAttachment;
        bool usesSlider = false;
        bool usesChoice = false;
        bool usesToggle = false;
        String localizationKey;
        String parameterId;
    };

public:
    AVEffectSettingsPanel(AudioProcessorValueTreeState& state, AVEffectKind kind)
        : parameters(state) {
        auto& language = AVDexedLanguageService::getInstance();
        language.addChangeListener(this);
        setLookAndFeel(&settingsLookAndFeel);
        headingKey = kind == AVEffectKind::reverb ? "reverbSettings"
                   : kind == AVEffectKind::delay ? "delaySettings" : "chorusSettings";
        const auto heading = language.text(headingKey);
        title.setText(heading, dontSendNotification);
        title.setFont(AVDexedTheme::font(20.0f, "SemiBold"));
        title.setColour(Label::textColourId, AVDexedTheme::textPrimary);
        addAndMakeVisible(title);

        if (kind == AVEffectKind::reverb) {
            addToggle("bypass", IDs::avReverbBypass.name);
            addSlider("sizeDecay", IDs::reverbSize.name);
            addSlider("preDelay", IDs::reverbPreDelay.name, " ms");
            addSlider("damping", IDs::reverbDamping.name);
        } else if (kind == AVEffectKind::delay) {
            addToggle("bypass", IDs::avDelayBypass.name);
            addToggle("tempoSync", IDs::delaySync.name);
            addSlider("localTempo", IDs::avDelayTempo.name, " BPM");
            addChoice("leftDivision", IDs::delayDivisionLeft.name, { "1/16", "1/8", "1/8D", "1/8T", "1/4", "1/4D", "1/4T", "1/2" });
            addChoice("rightDivision", IDs::delayDivisionRight.name, { "1/16", "1/8", "1/8D", "1/8T", "1/4", "1/4D", "1/4T", "1/2" });
            addSlider("leftTime", IDs::delayTimeLeft.name, " ms");
            addSlider("rightTime", IDs::delayTimeRight.name, " ms");
            addSlider("feedback", IDs::delayFeedback.name);
            addSlider("damping", IDs::delayDamping.name);
        } else {
            addToggle("bypass", IDs::avChorusBypass.name);
            addChoice("mode", IDs::chorusMode.name, { "I", "II", "I + II" });
            addSlider("rate", IDs::chorusRate.name, " Hz");
            addSlider("depth", IDs::chorusDepth.name, " ms");
            addSlider("stereoSpread", IDs::chorusSpread.name);
        }

        const int panelHeight = 76 + static_cast<int>(rows.size()) * 42 + 22;
        setSize(620, panelHeight);
        updateDelayTimingControls();
        setTitle(heading);
    }

    ~AVEffectSettingsPanel() override {
        AVDexedLanguageService::getInstance().removeChangeListener(this);
        setLookAndFeel(nullptr);
    }

    void paint(Graphics& g) override {
        // GOZO modal: panel gradient, no outline or glow; the scrim does the separation.
        AVDexedTheme::panel(g, getLocalBounds().toFloat(), 10.0f);
    }

    void resized() override {
        title.setBounds(26, 20, getWidth() - 80, 34);
        int y = 76;
        for (auto& row : rows) {
            row->label.setBounds(26, y, 210, 32);
            if (row->usesSlider) row->slider.setBounds(244, y, getWidth() - 270, 32);
            if (row->usesChoice) row->choice.setBounds(244, y + 1, getWidth() - 270, 30);
            if (row->usesToggle) row->toggle.setBounds(244, y + 3, 52, 26);
            y += 42;
        }
    }

private:
    void changeListenerCallback(ChangeBroadcaster*) override {
        auto& language = AVDexedLanguageService::getInstance();
        title.setText(language.text(headingKey), dontSendNotification);
        setTitle(language.text(headingKey));
        if (auto* dialog = findParentComponentOfClass<DialogWindow>())
            dialog->setName(language.text(headingKey));
        for (auto& row : rows) {
            const auto translated = language.text(row->localizationKey);
            row->label.setText(translated, dontSendNotification);
            row->slider.setTitle(translated);
            row->choice.setTitle(translated);
            row->toggle.setTitle(translated);
            if (row->usesToggle)
                row->toggle.setTooltip(language.text(row->parameterId == IDs::delaySync.name ? "syncToHost" : "bypass"));
        }
    }

    void setupRow(Row& row, const String& localizationKey) {
        row.localizationKey = localizationKey;
        row.label.setMinimumHorizontalScale(1.0f);
        const auto labelText = AVDexedLanguageService::getInstance().text(localizationKey);
        row.label.setText(labelText, dontSendNotification);
        row.label.setFont(AVDexedTheme::font(14.0f, "Medium"));
        row.label.setColour(Label::textColourId, AVDexedTheme::textSecondary);
        addAndMakeVisible(row.label);
    }

    void addSlider(const String& localizationKey, const String& parameter, const String& suffix = {}) {
        auto row = std::make_unique<Row>();
        setupRow(*row, localizationKey);
        row->usesSlider = true;
        row->parameterId = parameter;
        row->slider.setSliderStyle(Slider::LinearHorizontal);
        row->slider.setTextBoxStyle(Slider::TextBoxRight, false, 96, 28);
        row->slider.setVelocityBasedMode(false);
        row->slider.setMouseDragSensitivity(180);
        row->slider.setTextValueSuffix(suffix);
        row->slider.setColour(Slider::textBoxTextColourId, AVDexedTheme::textPrimary);
        row->slider.setColour(Slider::textBoxBackgroundColourId, AVDexedTheme::textPrimary.withAlpha(0.10f));
        row->slider.setColour(Slider::textBoxOutlineColourId, Colours::transparentBlack);
        row->slider.setTitle(AVDexedLanguageService::getInstance().text(localizationKey));
        addAndMakeVisible(row->slider);
        row->sliderAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(parameters, parameter, row->slider);
        if (suffix.isEmpty()) {
            row->slider.textFromValueFunction = [](double value) { return String(value * 100.0, 1) + "%"; };
            row->slider.valueFromTextFunction = [](const String& value) { return value.getDoubleValue() / 100.0; };
            row->slider.updateText();
        }
        if (auto* value = parameters.getParameter(parameter))
            row->slider.setDoubleClickReturnValue(true, value->convertFrom0to1(value->getDefaultValue()));
        rows.push_back(std::move(row));
    }

    void addChoice(const String& localizationKey, const String& parameter, const StringArray& items) {
        auto row = std::make_unique<Row>();
        setupRow(*row, localizationKey);
        row->usesChoice = true;
        row->parameterId = parameter;
        row->choice.addItemList(items, 1);
        row->choice.setTitle(AVDexedLanguageService::getInstance().text(localizationKey));
        row->choice.setColour(ComboBox::backgroundColourId, AVDexedTheme::controlBottom);
        row->choice.setColour(ComboBox::textColourId, AVDexedTheme::textPrimary);
        addAndMakeVisible(row->choice);
        row->choiceAttachment = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment>(parameters, parameter, row->choice);
        rows.push_back(std::move(row));
    }

    void addToggle(const String& localizationKey, const String& parameter) {
        auto row = std::make_unique<Row>();
        setupRow(*row, localizationKey);
        row->usesToggle = true;
        row->parameterId = parameter;
        row->toggle.setButtonText({});
        row->toggle.setTooltip(AVDexedLanguageService::getInstance().text(parameter == IDs::delaySync.name ? "syncToHost" : "bypass"));
        row->toggle.onStateChange = [this] { updateDelayTimingControls(); };
        row->toggle.setTitle(AVDexedLanguageService::getInstance().text(localizationKey));
        row->buttonAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment>(parameters, parameter, row->toggle);
        addAndMakeVisible(row->toggle);
        rows.push_back(std::move(row));
    }

    void updateDelayTimingControls() {
        const auto* syncParameter = parameters.getRawParameterValue(IDs::delaySync.name);
        const bool sync = syncParameter != nullptr && syncParameter->load() >= 0.5f;
        for (auto& row : rows) {
            const bool division = row->parameterId == IDs::delayDivisionLeft.name
                || row->parameterId == IDs::delayDivisionRight.name;
            const bool time = row->parameterId == IDs::delayTimeLeft.name
                || row->parameterId == IDs::delayTimeRight.name;
            if (division || time) {
                const bool enabled = division ? sync : !sync;
                row->label.setAlpha(enabled ? 1.0f : 0.5f);
                row->slider.setEnabled(enabled);
                row->choice.setEnabled(enabled);
            }
        }
    }

    AudioProcessorValueTreeState& parameters;
    AVDexedPlayLookAndFeel settingsLookAndFeel;
    Label title;
    std::vector<std::unique_ptr<Row>> rows;
    String headingKey;
};

class AVEffectOverlay final : public Component, private ComponentListener {
public:
    AVEffectOverlay(AudioProcessorValueTreeState& parameters, AVEffectKind kind, Component& parent,
                    std::function<void()> close)
        : AVParent(&parent), AVClose(std::move(close)), AVPanel(parameters, kind),
          AVCloseButton("AVCloseEffect", DrawableButton::ImageFitted) {
        const auto source = parent.createComponentSnapshot(parent.getLocalBounds());
        const auto small = source.rescaled(jmax(1, source.getWidth() / 3), jmax(1, source.getHeight() / 3));
        AVBackdrop = small.createCopy();
        ImageConvolutionKernel kernel(9); kernel.createGaussianBlur(2.0f);
        kernel.applyToImage(AVBackdrop, small, small.getBounds());
        addAndMakeVisible(AVPanel);
        AVPanel.addAndMakeVisible(AVCloseButton);
        if (auto icon = avDrawableFromSvg(BinaryData::AVDexed_CloseIcon_svg, BinaryData::AVDexed_CloseIcon_svgSize)) {
            auto blue = XmlDocument::parse(String::fromUTF8(BinaryData::AVDexed_CloseIcon_svg, BinaryData::AVDexed_CloseIcon_svgSize)
                .replace("#E0E0E0", "#" + AVDexedTheme::oceanTop.toDisplayString(false)).replace("#9A9A9A", "#" + AVDexedTheme::oceanBottom.toDisplayString(false)));
            auto hover = blue ? Drawable::createFromSVG(*blue) : nullptr;
            AVCloseButton.setImages(icon.get(), hover.get(), hover.get());
        }
        AVCloseButton.setEdgeIndent(3);
        AVCloseButton.setTooltip(AVDexedLanguageService::getInstance().text("close"));
        AVCloseButton.onClick = AVClose;
        parent.addComponentListener(this);
        setWantsKeyboardFocus(true);
        setFocusContainerType(FocusContainerType::focusContainer);
        setBounds(parent.getLocalBounds());
    }
    ~AVEffectOverlay() override { if (AVParent) AVParent->removeComponentListener(this); }
    void paint(Graphics& g) override {
        g.reduceClipRegion(0, 0, getWidth(), getHeight() - 30);  // keep the help footer visible
        g.drawImage(AVBackdrop, getLocalBounds().toFloat());
        g.setColour(Colours::black.withAlpha(0.54f)); g.fillAll();
    }
    bool hitTest(int, int y) override { return y < getHeight() - 30; }
    void resized() override {
        AVPanel.setCentrePosition(getLocalBounds().withTrimmedBottom(30).getCentre());
        AVCloseButton.setBounds(AVPanel.getWidth() - 44, 24, 19, 19);
    }
    void mouseDown(const MouseEvent& e) override {
        if (!AVPanel.getBounds().contains(e.getPosition()) && AVClose) AVClose();
    }
    void inputAttemptWhenModal() override { if (AVClose) AVClose(); }
    bool keyPressed(const KeyPress& key) override {
        if (key == KeyPress::escapeKey) { if (AVClose) AVClose(); return true; }
        return false;
    }
private:
    void componentMovedOrResized(Component&, bool, bool resized) override {
        if (resized && AVParent) setBounds(AVParent->getLocalBounds());
    }
    Component::SafePointer<Component> AVParent;
    std::function<void()> AVClose;
    Image AVBackdrop;
    AVEffectSettingsPanel AVPanel;
    DrawableButton AVCloseButton;
};

class AVFolderNameDialog final : public AlertWindow {
public:
    AVFolderNameDialog(const String& title, const String& message)
        : AlertWindow(title, message, AlertWindow::NoIcon) { setLookAndFeel(&lookAndFeel); }
    ~AVFolderNameDialog() override { setLookAndFeel(nullptr); }
private:
    AVDexedPlayLookAndFeel lookAndFeel;
};
}

AVDexedFolderPickerButton::AVDexedFolderPickerButton() {
    setButtonText({});
    setWantsKeyboardFocus(true);
    folderIcon = avDrawableFromSvg(BinaryData::AVDexed_FolderIcon_svg, BinaryData::AVDexed_FolderIcon_svgSize);
    favoriteIcon = avDrawableFromSvg(BinaryData::AVDexed_FavoriteIcon_svg, BinaryData::AVDexed_FavoriteIcon_svgSize);
}

void AVDexedFolderPickerButton::setSelection(const String& name, bool isFavoritesFolder) {
    folderName = name;
    favoritesSelected = isFavoritesFolder;
    setTitle(name);
    repaint();
}

void AVDexedFolderPickerButton::paintButton(Graphics& g, bool isMouseOverButton, bool isButtonDown) {
    // Same shape as a GOZO combo: 12% box (lift on hover), grey text, ocean chevron.
    const auto bounds = getLocalBounds().toFloat().reduced(0.5f);
    AVDexedTheme::fillBox(g, bounds, 6.0f, isMouseOverButton || isButtonDown);
    if (AVDexedTheme::showsFocus(*this)) {
        g.setColour(AVDexedTheme::oceanHighlight);
        g.drawRoundedRectangle(bounds, 6.0f, 1.2f);
    }
    const auto iconBounds = Rectangle<float>(14.0f, 14.0f).withPosition(bounds.getX() + 10.0f, bounds.getCentreY() - 7.0f);
    if (auto* icon = favoritesSelected ? favoriteIcon.get() : folderIcon.get())
        icon->drawWithin(g, iconBounds, RectanglePlacement::centred, 0.95f);
    AVDexedTheme::drawGreyText(g, folderName, AVDexedTheme::font(14.0f, "Medium"),
                               bounds.withTrimmedLeft(32.0f).withTrimmedRight(28.0f), Justification::centredLeft);
    const float cx = bounds.getRight() - 14.0f, cy = bounds.getCentreY() - 1.0f;
    Path chevron;
    chevron.startNewSubPath(cx - 5.0f, cy - 2.5f);
    chevron.lineTo(cx, cy + 2.5f);
    chevron.lineTo(cx + 5.0f, cy - 2.5f);
    g.setColour(AVDexedTheme::oceanHighlight);
    g.strokePath(chevron, PathStrokeType(2.0f, PathStrokeType::curved, PathStrokeType::rounded));
}

AVDexedActionIconButton::AVDexedActionIconButton(Glyph icon) : glyph(icon) {
    setButtonText({});
    setWantsKeyboardFocus(true);
    setRepaintsOnMouseActivity(true);
    if (glyph == Glyph::favorite) {
        AVIconActive = avDrawableFromSvg(BinaryData::AVDexed_FavoriteIcon_svg, BinaryData::AVDexed_FavoriteIcon_svgSize);
        AVIcon = avGreyDrawableFromSvg(BinaryData::AVDexed_FavoriteIcon_svg, BinaryData::AVDexed_FavoriteIcon_svgSize);
    } else {
        AVIcon = avDrawableFromSvg(glyph == Glyph::previous ? BinaryData::AVDexed_PreviousPreset_svg : BinaryData::AVDexed_NextPreset_svg,
                                  glyph == Glyph::previous ? BinaryData::AVDexed_PreviousPreset_svgSize : BinaryData::AVDexed_NextPreset_svgSize);
        if (AVIcon != nullptr) {
            auto path = AVIcon->getOutlineAsPath();
            path.applyTransform(AffineTransform::rotation(glyph == Glyph::previous
                ? -MathConstants<float>::halfPi : MathConstants<float>::halfPi));
            path.applyTransform(AffineTransform::translation(-path.getBounds().getX(), -path.getBounds().getY()));
            auto rotated = std::make_unique<DrawablePath>();
            rotated->setPath(path);
            rotated->setFill(AVDexedTheme::textPrimary);
            AVIcon = std::move(rotated);
            AVIconActive = AVIcon->createCopy();
            AVIconActive->replaceColour(AVDexedTheme::textPrimary, AVDexedTheme::oceanHighlight);
        }
    }
}

void AVDexedActionIconButton::paintButton(Graphics& g, bool over, bool down) {
    const auto bounds = getLocalBounds().toFloat().reduced(2.0f);
    if (AVDexedTheme::showsFocus(*this)) {
        g.setColour(AVDexedTheme::oceanHighlight);
        g.drawRoundedRectangle(bounds, 7.0f, 1.4f);
    }
    // The star is ocean only when it is on; hovering an off star brightens it.
    auto* drawable = glyph == Glyph::favorite ? (getToggleState() ? AVIconActive.get() : AVIcon.get())
                                              : (getToggleState() || over ? AVIconActive.get() : AVIcon.get());
    if (drawable != nullptr)
        drawable->drawWithin(g, bounds.withSizeKeepingCentre(glyph == Glyph::favorite ? 21.0f : 14.0f,
                                                            glyph == Glyph::favorite ? 21.0f : 22.0f),
                             RectanglePlacement::centred,
                             !isEnabled() ? 0.35f : down ? 0.72f
                             : glyph == Glyph::favorite && !getToggleState() && !over ? 0.55f : 1.0f);
}

void AVDexedPlayLookAndFeel::drawRotarySlider(Graphics& g, int x, int y, int width, int height,
                                             float position, float startAngle, float endAngle, Slider& slider) {
    ignoreUnused(startAngle, endAngle);
    AVDrawKnob(g, Rectangle<float>(float(x), float(y), float(width), float(height)).reduced(2.0f), position, slider);
}

class AVDexedPresetNavigator::NameButton final : public Button {
public:
    NameButton() : Button("presetName") {
        setRepaintsOnMouseActivity(true);
        setWantsKeyboardFocus(true);
    }
    String text;
    static Font nameFont() { return AVDexedTheme::font(17.0f, "Medium"); }
    void paintButton(Graphics& g, bool over, bool down) override {
        const auto area = getLocalBounds().toFloat();
        if (over || down) AVDexedTheme::drawOceanText(g, text, nameFont(), area);
        else AVDexedTheme::drawGreyText(g, text, nameFont(), area);
        if (AVDexedTheme::showsFocus(*this)) {
            g.setColour(AVDexedTheme::oceanHighlight);
            g.drawRoundedRectangle(area.reduced(0.5f), 6.0f, 1.2f);
        }
    }
};

AVDexedPresetNavigator::AVDexedPresetNavigator() : name(std::make_unique<NameButton>()) {
    for (auto* child : std::initializer_list<Component*>{ &previous, &next, &star, name.get() })
        addAndMakeVisible(child);
    previous.onClick = [this] { if (onPrevious) onPrevious(); };
    next.onClick = [this] { if (onNext) onNext(); };
    star.onClick = [this] { if (onFavorite) onFavorite(); };
    name->onClick = [this] { if (onName) onName(); };
    applyLanguage();
}

AVDexedPresetNavigator::~AVDexedPresetNavigator() = default;

void AVDexedPresetNavigator::applyLanguage() {
    auto& language = AVDexedLanguageService::getInstance();
    previous.setTitle(language.text("previous"));
    previous.setTooltip(language.text("previous"));
    next.setTitle(language.text("next"));
    next.setTooltip(language.text("next"));
    star.setTitle(language.text("favorite"));
    star.setTooltip(language.text("favorite"));
    name->setTooltip(language.text("presetNameHelp"));
}

void AVDexedPresetNavigator::setPreset(const String& newName, const String& status, bool modified,
                                       bool favorite, bool favoriteAvailable) {
    if (presetName == newName && statusText == status && presetModified == modified
        && star.getToggleState() == favorite && star.isEnabled() == favoriteAvailable)
        return;
    presetName = newName;
    statusText = status;
    presetModified = modified;
    name->text = newName;
    name->setTitle(newName);
    star.setToggleState(favorite, dontSendNotification);
    star.setEnabled(favoriteAvailable);
    layoutStar();
    repaint();
}

void AVDexedPresetNavigator::resized() {
    layoutStar();
}

void AVDexedPresetNavigator::layoutStar() {
    // Name and star travel together as one centred group between the arrows.
    const int starSize = 26, gap = 4;
    const int available = jmax(40, getWidth() - 2 * 40 - starSize - gap);
    const int nameWidth = jmin(available, roundToInt(GlyphArrangement::getStringWidth(NameButton::nameFont(), presetName)) + 10);
    const int x = (getWidth() - (nameWidth + gap + starSize)) / 2;
    name->setBounds(x, 6, nameWidth, 30);
    star.setBounds(x + nameWidth + gap, 8, starSize, starSize);
    // ‹ › sit right beside the name, as in GOZO, not at the far ends.
    previous.setBounds(jmax(0, x - 34), 4, 26, 34);
    next.setBounds(jmin(getWidth() - 26, star.getRight() + 8), 4, 26, 34);
}

void AVDexedPresetNavigator::paint(Graphics& g) {
    // Status line: source in grey, "Modified" in ocean so the state reads as
    // a word, not only a colour.
    const auto font = AVDexedTheme::font(12.0f, "Medium");
    const auto separator = statusText.isEmpty() ? String() : String::fromUTF8(" · ");
    const auto modified = presetModified ? AVDexedLanguageService::getInstance().text("modifiedShort") : String();
    const float statusWidth = GlyphArrangement::getStringWidth(font, statusText);
    const float modifiedWidth = modified.isEmpty() ? 0.0f : GlyphArrangement::getStringWidth(font, separator + modified);
    const auto line = Rectangle<float>(30.0f, 38.0f, float(getWidth() - 60), 18.0f);
    const float total = jmin(line.getWidth(), statusWidth + modifiedWidth);
    const float x = line.getCentreX() - total * 0.5f;
    const float statusSpace = jmax(0.0f, total - modifiedWidth);
    AVDexedTheme::drawGreyText(g, statusText, font, { x, line.getY(), statusSpace, line.getHeight() },
                               Justification::centredLeft, 0.85f);
    if (modified.isNotEmpty())
        AVDexedTheme::drawOceanText(g, separator + modified, font,
                                    { x + statusSpace, line.getY(), modifiedWidth + 2.0f, line.getHeight() },
                                    Justification::centredLeft);
}

AVDexedValueTip::AVDexedValueTip() {
    setVisible(false);
    setMouseCursor(MouseCursor::IBeamCursor);
}

void AVDexedValueTip::show(Slider& target, const String& newText) {
    auto* parent = getParentComponent();
    if (parent == nullptr) return;
    // Leaving the field by touching a knob applies the typed value, the same
    // as clicking anywhere else.
    if (editor != nullptr) finishEditing(true, false);  // the touched knob keeps focus
    slider = &target;
    text = newText;
    held = true;
    holdFrames = 72;
    const auto font = AVDexedTheme::font(15.0f, "Medium");
    const int width = jmax(jmax(59, getWidth()), roundToInt(GlyphArrangement::getStringWidth(font, text)) + 26);
    const int height = 24;
    const auto knob = parent->getLocalArea(&target, target.getLocalBounds());
    int y = knob.getY() - 15 - height;
    if (y < 0) y = knob.getBottom() + 15;
    setBounds(knob.getCentreX() - width / 2, y, width, height);
    setVisible(true);
    toFront(false);
    startTimerHz(60);
    repaint();
}

void AVDexedValueTip::release() {
    held = false;
    holdFrames = 72;
}

void AVDexedValueTip::mouseUp(const MouseEvent& event) {
    if (!event.mouseWasDraggedSinceMouseDown() && slider != nullptr && editor == nullptr)
        beginEditing();
}

void AVDexedValueTip::beginEditing() {
    editor = std::make_unique<TextEditor>();
    editor->setFont(AVDexedTheme::font(15.0f, "Medium"));
    editor->setJustification(Justification::centred);
    editor->setIndents(4, 2);
    editor->setColour(TextEditor::backgroundColourId, Colours::transparentBlack);
    editor->setColour(TextEditor::outlineColourId, Colours::transparentBlack);
    editor->setColour(TextEditor::focusedOutlineColourId, Colours::transparentBlack);
    editor->setColour(TextEditor::textColourId, AVDexedTheme::textPrimary);
    editor->setColour(TextEditor::highlightColourId, AVDexedTheme::oceanTop.withAlpha(0.35f));
    editor->setColour(CaretComponent::caretColourId, AVDexedTheme::oceanHighlight);
    editor->setText(text, false);
    editor->selectAll();
    editor->onReturnKey = [this] { finishEditing(true); };
    editor->onEscapeKey = [this] { finishEditing(false); };
    editor->onFocusLost = [this] { finishEditing(true, false); };  // focus already went elsewhere
    addAndMakeVisible(*editor);
    editor->setBounds(getLocalBounds().reduced(4, 2));
    editor->grabKeyboardFocus();
    held = true;
}

void AVDexedValueTip::finishEditing(bool commit, bool returnFocusToKnob) {
    if (editor == nullptr) return;
    const auto typed = editor->getText();
    // Defer the editor's destruction: this runs inside its own callbacks.
    auto* finished = editor.release();
    // Hand focus back to the knob first; hiding a focused editor would let JUCE
    // pass focus to the first focusable sibling (the search field).
    if (returnFocusToKnob && slider != nullptr && slider->isShowing())
        slider->grabKeyboardFocus();
    finished->setVisible(false);
    MessageManager::callAsync([finished] { delete finished; });
    if (commit && slider != nullptr && typed.trim().isNotEmpty()) {
        slider->setValue(slider->getValueFromText(typed), sendNotificationSync);
        text = slider->getTextFromValue(slider->getValue());
    }
    release();
    repaint();
}

void AVDexedValueTip::timerCallback() {
    if (held || holdFrames > 0) {
        opacity = jmin(1.0f, opacity + 1.0f / 5.0f);
        if (!held) --holdFrames;
    } else {
        opacity -= 1.0f / 10.0f;
        if (opacity <= 0.0f) {
            opacity = 0.0f;
            stopTimer();
            setVisible(false);
            setSize(0, 0);
            slider = nullptr;
        }
    }
    repaint();
}

void AVDexedValueTip::paint(Graphics& g) {
    const auto area = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(Colours::black.withAlpha(0.55f * opacity));
    g.fillRoundedRectangle(area, 6.0f);
    ColourGradient glass(Colours::white.withAlpha(0.15f * opacity), 0.0f, area.getY(),
                         Colours::white.withAlpha(0.15f * opacity), 0.0f, area.getBottom(), false);
    glass.addColour(0.5, Colours::transparentWhite);
    g.setGradientFill(glass);
    g.fillRoundedRectangle(area, 6.0f);
    g.setColour(Colours::white.withAlpha(0.3f * opacity));
    g.drawRoundedRectangle(area, 6.0f, 1.0f);
    if (editor == nullptr)
        AVDexedTheme::drawGreyText(g, text, AVDexedTheme::font(15.0f, "Medium"), area, Justification::centred, opacity);
}

AVDexedKnobCaption::AVDexedKnobCaption(bool settings) : Button("caption"), opensSettings(settings) {
    setRepaintsOnMouseActivity(opensSettings);
    setWantsKeyboardFocus(opensSettings);
    setInterceptsMouseClicks(opensSettings, false);
    if (opensSettings) setMouseCursor(MouseCursor::PointingHandCursor);
    // Plain captions only repeat the knob's own accessible title.
    else setAccessible(false);
}

void AVDexedKnobCaption::paintButton(Graphics& g, bool over, bool down) {
    const auto font = AVDexedTheme::font(16.0f, "Medium");
    const bool lit = opensSettings && isEnabled() && (over || down);
    const float alpha = isEnabled() ? 1.0f : 0.5f;
    const float chevron = opensSettings ? 14.0f : 0.0f;
    const float textWidth = jmin(float(getWidth()) - chevron, GlyphArrangement::getStringWidth(font, getButtonText()) + 2.0f);
    const float x = (float(getWidth()) - textWidth - chevron) * 0.5f;
    const auto textArea = Rectangle<float>(x, 0.0f, textWidth, float(getHeight()));
    if (lit) AVDexedTheme::drawOceanText(g, getButtonText(), font, textArea, Justification::centredLeft, alpha);
    else AVDexedTheme::drawGreyText(g, getButtonText(), font, textArea, Justification::centredLeft, alpha);
    if (opensSettings) {
        const float cx = textArea.getRight() + 8.0f, cy = float(getHeight()) * 0.5f;
        Path mark;
        mark.startNewSubPath(cx - 4.0f, cy - 2.0f);
        mark.lineTo(cx, cy + 2.0f);
        mark.lineTo(cx + 4.0f, cy - 2.0f);
        g.setColour((lit ? AVDexedTheme::oceanHighlight : AVDexedTheme::textSecondary).withMultipliedAlpha(alpha));
        g.strokePath(mark, PathStrokeType(1.8f, PathStrokeType::curved, PathStrokeType::rounded));
    }
    if (AVDexedTheme::showsFocus(*this)) {
        g.setColour(AVDexedTheme::oceanHighlight);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.0f, 1.2f);
    }
}

AVDexedPlayView::AVDexedPlayView(DexedAudioProcessor& owner) : processor(owner) {
    AVDexedLanguageService::getInstance().addChangeListener(this);
    avBrowserPreferenceChanges.addChangeListener(this);
    setLookAndFeel(&playLookAndFeel);
    favoriteRowIcon = avDrawableFromSvg(BinaryData::AVDexed_FavoriteIcon_svg, BinaryData::AVDexed_FavoriteIcon_svgSize);
    favoriteRowIconOff = avGreyDrawableFromSvg(BinaryData::AVDexed_FavoriteIcon_svg, BinaryData::AVDexed_FavoriteIcon_svgSize);
    folderRowIcon = avDrawableFromSvg(BinaryData::AVDexed_FolderIcon_svg, BinaryData::AVDexed_FolderIcon_svgSize);
    loadFolderCatalog();
    applyLanguage();
    browserTitle.setFont(AVDexedTheme::font(16.0f, "SemiBold"));
    browserTitle.setColour(Label::textColourId, AVDexedTheme::textPrimary);
    addAndMakeVisible(browserTitle);

    searchField.setFont(AVDexedTheme::font(14.0f, "Medium"));
    searchField.setIndents(4, 8);
    searchField.setEscapeAndReturnKeysConsumed(true);
    searchField.setSelectAllWhenFocused(true);
    // The capsule behind the field is painted by the view (GOZO search pill).
    searchField.setColour(TextEditor::backgroundColourId, Colours::transparentBlack);
    searchField.setColour(TextEditor::outlineColourId, Colours::transparentBlack);
    searchField.setColour(TextEditor::focusedOutlineColourId, Colours::transparentBlack);
    searchField.setColour(TextEditor::textColourId, AVDexedTheme::textPrimary);
    searchField.onTextChange = [this] { rebuildFilter(); };
    searchField.onLookChanged = [this] { repaint(searchBounds); };
    // clear() does not fire onTextChange, so the filter is rebuilt explicitly.
    searchField.onEscapeKey = [this] { searchField.clear(); rebuildFilter(); };
    addAndMakeVisible(searchField);

    for (auto* button : { &folderActionsButton, &importBankButton, &showActiveButton }) {
        button->getProperties().set("AVLink", true);
        button->setColour(TextButton::buttonColourId, AVDexedTheme::controlTop);
        button->setColour(TextButton::buttonOnColourId, AVDexedTheme::oceanBottom);
        button->setColour(TextButton::textColourOffId, AVDexedTheme::textSecondary);
        button->setColour(TextButton::textColourOnId, AVDexedTheme::textPrimary);
        addAndMakeVisible(*button);
    }
    folderPickerButton.setColour(TextButton::buttonColourId, AVDexedTheme::controlTop);
    folderPickerButton.setColour(TextButton::buttonOnColourId, AVDexedTheme::oceanBottom);
    folderPickerButton.setColour(TextButton::textColourOffId, AVDexedTheme::textSecondary);
    folderPickerButton.setColour(TextButton::textColourOnId, AVDexedTheme::textPrimary);
    addAndMakeVisible(folderPickerButton);
    folderPickerButton.onClick = [this] { showFolderPicker(); };
    folderActionsButton.onClick = [this] { showFolderActions(); };
    importBankButton.onClick = [this] { if (onImportBankRequested) onImportBankRequested(); };

    programList.setModel(this);
    programList.setRowHeight(34);
    programList.setOutlineThickness(0);
    programList.addMouseListener(this, true);
    programList.addKeyListener(&focusKeys);
    programList.onKeyboardSelection = [this](int row) { requestEntryLoad(row); };
    programList.setWantsKeyboardFocus(true);
    programList.setMultipleSelectionEnabled(false);
    programList.setColour(ListBox::backgroundColourId, Colours::transparentBlack);
    programList.setColour(ListBox::outlineColourId, Colours::transparentBlack);
    addAndMakeVisible(programList);
    for (auto* label : { &browserHelp, &browserCount, &emptyLibraryLabel }) {
        label->setFont(AVDexedTheme::font(10.0f, "Regular"));
        label->setColour(Label::textColourId, AVDexedTheme::textSecondary);
        label->setMinimumHorizontalScale(1.0f);
        addAndMakeVisible(*label);
    }
    browserHelp.setFont(AVDexedTheme::font(10.0f, "Regular"));
    browserHelp.setJustificationType(Justification::topLeft);
    emptyLibraryLabel.setJustificationType(Justification::centred);
    emptyLibraryLabel.setInterceptsMouseClicks(false, false);
    showActiveButton.onClick = [this] {
        searchField.clear();
        setSelectedFolder("all");
        selectActivePreset();
        programList.scrollToEnsureRowIsOnscreen(programList.getSelectedRow());
    };

    // Centre title between the knob trios, like the instrument name in GOZO.
    activeName.setFont(AVDexedTheme::font(26.0f, "Medium"));
    activeName.setColour(Label::textColourId, AVDexedTheme::textPrimary);
    activeName.setJustificationType(Justification::centred);
    activeName.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(activeName);
    sourceName.setFont(AVDexedTheme::font(13.0f, "Medium"));
    sourceName.setMinimumHorizontalScale(1.0f);
    sourceName.setJustificationType(Justification::centred);
    sourceName.setColour(Label::textColourId, AVDexedTheme::textSecondary);
    addAndMakeVisible(sourceName);
    modifiedLabel.setFont(AVDexedTheme::font(12.0f, "SemiBold"));
    modifiedLabel.setColour(Label::textColourId, AVDexedTheme::oceanHighlight);
    modifiedLabel.setJustificationType(Justification::centred);
    addAndMakeVisible(modifiedLabel);
    browserHelp.setVisible(false);

    presetNavigator.onPrevious = [this] { navigatePreset(-1); };
    presetNavigator.onNext = [this] { navigatePreset(1); };
    presetNavigator.onFavorite = [this] { toggleFavorite(); };
    presetNavigator.onName = [this] { showActiveButton.triggerClick(); };

    heroAlgorithm = std::make_unique<AlgoDisplay>(processor.parameters);
    heroAlgorithm->setHeroStyle(true);
    heroAlgorithm->onOperatorClicked = [this](int op) { if (onOperatorSelected) onOperatorSelected(op); };
    addAndMakeVisible(*heroAlgorithm);
    algorithmCaption.setFont(AVDexedTheme::font(15.0f, "SemiBold"));
    algorithmCaption.setColour(Label::textColourId, AVDexedTheme::textPrimary);
    algorithmCaption.setJustificationType(Justification::centred);
    algorithmCaption.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(algorithmCaption);

    for (size_t i = 0; i < knobCaptions.size(); ++i) {
        knobCaptions[i] = std::make_unique<AVDexedKnobCaption>(i >= 3);
        addAndMakeVisible(*knobCaptions[i]);
    }
    knobCaptions[3]->onClick = [this] { showEffectSettings(0, *knobCaptions[3]); };
    knobCaptions[4]->onClick = [this] { showEffectSettings(1, *knobCaptions[4]); };
    knobCaptions[5]->onClick = [this] { showEffectSettings(2, *knobCaptions[5]); };
    addChildComponent(valueTip);

    AVContextMenu = std::make_unique<AVQuickMidiContextMenu>(processor);
    volumeSlider.setName(IDs::masterVolume.name);
    panSlider.setName(IDs::pan.name);
    brightnessSlider.setName(IDs::cutoff.name);
    reverbSlider.setName(IDs::reverbAmount.name);
    delaySlider.setName(IDs::delayAmount.name);
    chorusSlider.setName(IDs::chorusAmount.name);
    for (auto* slider : { &volumeSlider, &panSlider, &brightnessSlider, &reverbSlider, &delaySlider, &chorusSlider }) {
        slider->setLookAndFeel(&playLookAndFeel);
        slider->setSliderStyle(Slider::RotaryHorizontalVerticalDrag);
        // GOZO knobs carry no text box: the value appears in a tip while dragging
        // and in the footer on hover.
        slider->setTextBoxStyle(Slider::NoTextBox, false, 0, 0);
        slider->setWantsKeyboardFocus(true);
        slider->setMouseDragSensitivity(150);
        slider->onDragStart = [this, slider] { showValueTip(*slider); };
        slider->onValueChange = [this, slider] { if (slider->isMouseButtonDown()) showValueTip(*slider); };
        slider->onDragEnd = [this] { valueTip.release(); };
        slider->addMouseListener(AVContextMenu.get(), true);
        slider->setColour(Slider::rotarySliderFillColourId, AVDexedTheme::oceanTop);
        slider->setColour(Slider::rotarySliderOutlineColourId, AVDexedTheme::outline);
        slider->setColour(Slider::textBoxTextColourId, AVDexedTheme::textPrimary);
        slider->setColour(Slider::textBoxBackgroundColourId, Colours::transparentBlack);
        slider->setColour(Slider::textBoxOutlineColourId, Colours::transparentBlack);
        addAndMakeVisible(*slider);
    }
    volumeAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, IDs::masterVolume.name, volumeSlider);
    panAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, IDs::pan.name, panSlider);
    brightnessAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(
        processor.parameters, IDs::cutoff.name, brightnessSlider);
    brightnessSlider.textFromValueFunction = [](double value) { return String(roundToInt(value * 100.0)) + "%"; };
    brightnessSlider.valueFromTextFunction = [](const String& text) { return text.getDoubleValue() / 100.0; };
    brightnessSlider.setDoubleClickReturnValue(true, 1.0);
    reverbAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(processor.parameters, IDs::reverbAmount.name, reverbSlider);
    delayAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(processor.parameters, IDs::delayAmount.name, delaySlider);
    chorusAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(processor.parameters, IDs::chorusAmount.name, chorusSlider);
    volumeSlider.textFromValueFunction = [](double value) {
        return value <= 0.00001 ? String::fromUTF8("−∞ dB") : String(Decibels::gainToDecibels(value), 1) + " dB";
    };
    volumeSlider.valueFromTextFunction = [](const String& text) {
        if (text.containsChar(0x221e) || text.containsIgnoreCase("inf")) return 0.0;
        return Decibels::decibelsToGain(text.getDoubleValue());
    };
    volumeSlider.setDoubleClickReturnValue(true, 1.0);
    volumeSlider.setSkewFactorFromMidPoint(1.0);
    panSlider.getProperties().set("AVBipolar", true);
    panSlider.textFromValueFunction = [](double value) {
        if (std::abs(value - 0.5) < 0.005) return String("C");
        return String(value < 0.5 ? "L " : "R ") + String(static_cast<int>(std::round(std::abs(value - 0.5) * 200.0)));
    };
    panSlider.valueFromTextFunction = [](const String& text) {
        const auto trimmed = text.trim().toUpperCase();
        if (trimmed == "C") return 0.5;
        const double amount = trimmed.retainCharacters("0123456789.-").getDoubleValue() / 200.0;
        return trimmed.startsWith("L") ? 0.5 - amount : trimmed.startsWith("R") ? 0.5 + amount
            : (trimmed.getDoubleValue() + 100.0) / 200.0;
    };
    panSlider.setDoubleClickReturnValue(true, 0.5);
    for (auto* slider : { &reverbSlider, &delaySlider, &chorusSlider }) {
        slider->textFromValueFunction = [](double value) { return String(static_cast<int>(std::round(value * 100.0))) + "%"; };
        slider->valueFromTextFunction = [](const String& text) { return text.getDoubleValue() / 100.0; };
        slider->setDoubleClickReturnValue(true, 0.0);
        slider->updateText();
    }
    volumeSlider.updateText();
    panSlider.updateText();
    rebuildFolderPicker();
    updateBank();
    applyLanguage();
}

void AVDexedPlayView::changeListenerCallback(ChangeBroadcaster* source) {
    if (source == &avBrowserPreferenceChanges) {
        const auto previousBanks = knownBankPaths;
        const auto previousPresets = knownPresetPaths;
        loadFolderCatalog();
        loadFavorites();
        rebuildFolderPicker();
        rebuildFilter();
        refreshLabels();
        if (knownBankPaths != previousBanks || knownPresetPaths != previousPresets)
            reindexLibrary();
    } else {
        applyLanguage();
    }
    repaint();
}

void AVDexedPlayView::applyLanguage() {
    auto& language = AVDexedLanguageService::getInstance();
    browserTitle.setText(language.text("sounds"), dontSendNotification);
    searchField.setTextToShowWhenEmpty(language.text("search"), AVDexedTheme::textSecondary);
    searchField.setTitle(language.text("search"));
    searchField.setTooltip(language.text("browserHelp"));
    programList.setTooltip(language.text("browserHelp"));
    emptyLibraryLabel.setText(language.text("noPresets"), dontSendNotification);
    showActiveButton.setButtonText(language.text("goToActive"));
    showActiveButton.setTooltip(language.text("showActivePreset"));
    folderActionsButton.setButtonText(language.text("organize"));
    folderActionsButton.setTooltip(language.text("manageFolders"));
    importBankButton.setButtonText(language.text("importBank"));
    programList.setTitle(language.text("sounds"));
    presetNavigator.applyLanguage();
    volumeSlider.setTitle(language.text("volume"));
    panSlider.setTitle(language.text("pan"));
    brightnessSlider.setTitle(language.text("knobBrightness"));
    brightnessSlider.setTooltip(language.text("brightnessHelp"));
    reverbSlider.setTitle(language.text("knobReverb") + String::fromUTF8(" · ") + language.text("amount"));
    delaySlider.setTitle(language.text("knobDelay") + String::fromUTF8(" · ") + language.text("amount"));
    chorusSlider.setTitle(language.text("knobChorus") + String::fromUTF8(" · ") + language.text("amount"));
    const char* captionKeys[] { "knobVolume", "knobPan", "knobBrightness", "knobReverb", "knobDelay", "knobChorus" };
    for (size_t i = 0; i < knobCaptions.size() && knobCaptions[i] != nullptr; ++i) {
        const auto caption = language.text(captionKeys[i]);
        knobCaptions[i]->setButtonText(caption);
        knobCaptions[i]->setTitle(caption);
        if (i >= 3) knobCaptions[i]->setTooltip(language.format("effectSettingsHint", {{"effect", caption}}));
    }
    rebuildFolderPicker();
    rebuildFilter();
    refreshLabels();
}

AVDexedPlayView::~AVDexedPlayView() {
    programList.removeMouseListener(this);
    programList.removeKeyListener(&focusKeys);
    effectSettingsWindow.reset();
    if (libraryIndexCancelled != nullptr) libraryIndexCancelled->store(true);
    libraryIndexPool.removeAllJobs(true, 5000);
    AVDexedLanguageService::getInstance().removeChangeListener(this);
    avBrowserPreferenceChanges.removeChangeListener(this);
    setLookAndFeel(nullptr);
    for (auto* slider : { &volumeSlider, &panSlider, &brightnessSlider, &reverbSlider, &delaySlider, &chorusSlider }) {
        slider->removeMouseListener(AVContextMenu.get());
        slider->setLookAndFeel(nullptr);
    }
}

void AVDexedPlayView::showEffectSettings(int kind, Button& anchor) {
    ignoreUnused(anchor);
    effectSettingsWindow.reset();
    auto* parent = getParentComponent();
    if (parent == nullptr) return;
    const Component::SafePointer<AVDexedPlayView> safeThis(this);
    auto close = [safeThis] {
        if (!safeThis || !safeThis->effectSettingsWindow) return;
        auto* overlay = safeThis->effectSettingsWindow.get();
        Desktop::getInstance().getAnimator().cancelAnimation(overlay, false);
        overlay->exitModalState(0);
        // GOZO dialog timing: in ~5 frames, out ~7 frames.
        Desktop::getInstance().getAnimator().fadeOut(overlay, 117);
        Timer::callAfterDelay(1, [safeThis] {
            if (safeThis && safeThis->effectSettingsWindow && !safeThis->effectSettingsWindow->isVisible())
                safeThis->effectSettingsWindow.reset();
        });
    };
    effectSettingsWindow = std::make_unique<AVEffectOverlay>(processor.parameters, static_cast<AVEffectKind>(kind), *parent, close);
    parent->addChildComponent(*effectSettingsWindow);  // hidden, so fadeIn() animates
    effectSettingsWindow->enterModalState(true);
    Desktop::getInstance().getAnimator().fadeIn(effectSettingsWindow.get(), 83);
}

void AVDexedPlayView::visibilityChanged() {
    if (!isVisible()) effectSettingsWindow.reset();
}

void AVDexedPlayView::paint(Graphics& g) {
    g.fillAll(AVDexedTheme::canvas);
    // A soft ocean light behind the algorithm plays the role GOZO's
    // instrument photo plays: one focal point, no boxes around it.
    if (!heroBounds.isEmpty()) {
        const auto centre = heroBounds.toFloat().getCentre();
        ColourGradient glow(AVDexedTheme::oceanBottom.withAlpha(0.42f), centre.x, centre.y,
                            Colours::transparentBlack, centre.x + float(heroBounds.getWidth()) * 0.62f, centre.y, true);
        g.setGradientFill(glow);
        g.fillRect(getLocalBounds());
    }
    AVDexedTheme::panel(g, browserCard.toFloat(), 22.0f);
    AVDexedTheme::fillBox(g, searchBounds.toFloat(), 16.0f, searchField.hasKeyboardFocus(true) || searchField.isMouseOver(true));
    // Magnifier: ring and handle, drawn rather than taken from a font.
    const auto lens = Rectangle<float>(12.0f, 12.0f).withPosition(float(searchBounds.getX()) + 13.0f,
                                                                   float(searchBounds.getCentreY()) - 7.0f);
    g.setColour(AVDexedTheme::textSecondary);
    g.drawEllipse(lens, 1.6f);
    g.drawLine(lens.getRight() - 1.5f, lens.getBottom() - 1.5f, lens.getRight() + 3.0f, lens.getBottom() + 3.0f, 1.8f);
}

void AVDexedPlayView::resized() {
    if (heroAlgorithm == nullptr || knobCaptions.back() == nullptr) return;
    const int height = getHeight();
    // Browser: one GOZO card on the left, from the top bar to the knob row.
    browserCard = Rectangle<int>(30, 12, 360, jmax(200, height - 24));
    auto card = browserCard.reduced(20, 16);
    auto heading = card.removeFromTop(28);
    browserCount.setBounds(heading.removeFromRight(70));
    browserTitle.setBounds(heading);
    card.removeFromTop(10);
    searchBounds = card.removeFromTop(32);
    searchField.setBounds(searchBounds.withTrimmedLeft(30).withTrimmedRight(8));
    card.removeFromTop(10);
    auto folders = card.removeFromTop(30);
    folderActionsButton.setBounds(folders.removeFromRight(92));
    folders.removeFromRight(8);
    folderPickerButton.setBounds(folders);
    card.removeFromTop(10);
    auto links = card.removeFromBottom(28);
    importBankButton.setBounds(links.removeFromLeft(links.getWidth() / 2));
    showActiveButton.setBounds(links);
    card.removeFromBottom(8);
    programList.setBounds(card);
    emptyLibraryLabel.setBounds(card.reduced(16));

    // Hero: the algorithm as the page's single focal point.
    const int left = browserCard.getRight() + 20;
    const int right = getWidth() - 30;
    const int centreX = (left + right) / 2;
    const int knobRowTop = height - 168;
    heroBounds = Rectangle<int>(left, 0, right - left, knobRowTop - 14);
    algorithmCaption.setBounds(centreX - 260, 18, 520, 26);
    const int heroHeight = jmax(80, knobRowTop - 92);
    const int heroWidth = jmin(right - left - 80, roundToInt(float(heroHeight) * 152.0f / 91.0f));
    heroAlgorithm->setBounds(Rectangle<int>(heroWidth, roundToInt(float(heroWidth) * 91.0f / 152.0f))
                                 .withCentre({ centreX, 52 + (knobRowTop - 66) / 2 }));

    // Knob row: three output knobs, the sound's name, three effects.
    const std::array<Slider*, 6> knobs { &volumeSlider, &panSlider, &brightnessSlider,
                                         &reverbSlider, &delaySlider, &chorusSlider };
    const int cell = 116;
    const std::array<int, 6> centres { left + 60, left + 60 + cell, left + 60 + 2 * cell,
                                       right - 60 - 2 * cell, right - 60 - cell, right - 60 };
    for (size_t i = 0; i < knobs.size(); ++i) {
        knobs[i]->setBounds(centres[i] - 48, knobRowTop, 96, 96);
        knobCaptions[i]->setBounds(centres[i] - cell / 2, knobRowTop + 100, cell, 26);
    }
    centreTitleBounds = Rectangle<int>(centres[2] + 64, knobRowTop + 14, centres[3] - centres[2] - 128, 96);
    auto title = centreTitleBounds;
    activeName.setBounds(title.removeFromTop(40));
    sourceName.setBounds(title.removeFromTop(22));
    modifiedLabel.setBounds(title.removeFromTop(20));
}

int AVDexedPlayView::getNumRows() {
    return static_cast<int>(visibleEntries.size());
}

void AVDexedPlayView::paintListBoxItem(int row, Graphics& g, int width, int height, bool rowIsSelected) {
    if (!isPositiveAndBelow(row, static_cast<int>(visibleEntries.size()))) return;
    const auto& entry = visibleEntries[static_cast<size_t>(row)];
    const bool active = entry.key == activePresetKey();
    const auto rowArea = Rectangle<float>(2.0f, 1.0f, float(width - 4), float(height - 2));
    if (active) AVDexedTheme::fillRowHighlight(g, rowArea, 1.0f);
    // A row picked with the arrows (not yet loaded) always shows, ring or not.
    else if (rowIsSelected && programList.hasKeyboardFocus(true)) AVDexedTheme::fillRowHighlight(g, rowArea, 0.4f);
    else if (row == hoveredRow) AVDexedTheme::fillRowHighlight(g, rowArea, 0.21f);
    if (rowIsSelected && AVDexedTheme::showsFocus(programList)) {
        g.setColour(AVDexedTheme::oceanHighlight.withAlpha(0.8f));
        g.drawRoundedRectangle(rowArea.reduced(0.5f), 6.0f, 1.0f);
    }
    const auto number = entry.isUser ? String("U") : String(entry.program + 1).paddedLeft('0', 2);
    AVDexedTheme::drawGreyText(g, number, AVDexedTheme::font(12.0f, "Medium"),
                               { 12.0f, 0.0f, 26.0f, float(height) }, Justification::centredLeft, 0.6f);
    const float starX = float(width) - 30.0f;
    const auto sourceFont = AVDexedTheme::font(11.0f, "Medium");
    const float sourceWidth = jmin(96.0f, GlyphArrangement::getStringWidth(sourceFont, entry.source) + 4.0f);
    const auto nameArea = Rectangle<float>(42.0f, 0.0f, jmax(10.0f, starX - 52.0f - sourceWidth), float(height));
    if (active) AVDexedTheme::drawOceanText(g, entry.name, AVDexedTheme::font(15.0f, "SemiBold"), nameArea, Justification::centredLeft);
    else AVDexedTheme::drawGreyText(g, entry.name, AVDexedTheme::font(15.0f, "Medium"), nameArea, Justification::centredLeft);
    AVDexedTheme::drawGreyText(g, entry.source, sourceFont, { starX - 6.0f - sourceWidth, 0.0f, sourceWidth, float(height) },
                               Justification::centredRight, 0.55f);
    // Star: text-coloured when off (brighter on the hovered row), ocean when on.
    const bool favorite = favoriteKeys.contains(entry.key);
    if (favoriteRowIcon != nullptr) {
        const auto starBounds = Rectangle<float>(16.0f, 16.0f).withCentre({ starX + 10.0f, float(height) * 0.5f });
        if (favorite) {
            favoriteRowIcon->drawWithin(g, starBounds, RectanglePlacement::centred, 1.0f);
        } else if (favoriteRowIconOff != nullptr) {
            favoriteRowIconOff->drawWithin(g, starBounds, RectanglePlacement::centred, row == hoveredRow ? 0.75f : 0.32f);
        }
    }
}

void AVDexedPlayView::selectedRowsChanged(int) {
    // Programmatic selection (search refresh, "go to active", context menus)
    // only moves the highlight. Loads come from a click, Enter, the arrow keys
    // (AVDexedPresetList) or the previous/next buttons.
}

void AVDexedPlayView::requestEntryLoad(int row) {
    if (!isPositiveAndBelow(row, static_cast<int>(visibleEntries.size()))) return;
    const auto& entry = visibleEntries[static_cast<size_t>(row)];
    if (entry.key == activePresetKey()) return;
    if (entry.isUser) {
        if (onPresetLoadRequested) onPresetLoadRequested(entry.file);
    } else {
        selectProgram(row);
    }
}

void AVDexedPlayView::mouseMove(const MouseEvent& event) {
    int row = -1;
    if (event.eventComponent != this && programList.isParentOf(event.eventComponent)) {
        const auto position = programList.getLocalPoint(event.eventComponent, event.getPosition());
        row = programList.getRowContainingPosition(position.x, position.y);
    }
    if (row == hoveredRow) return;
    programList.repaintRow(hoveredRow);
    hoveredRow = row;
    programList.repaintRow(hoveredRow);
}

void AVDexedPlayView::mouseExit(const MouseEvent& event) {
    if (event.eventComponent == this || !programList.isParentOf(event.eventComponent)) return;
    // Moving between rows keeps the pointer over the list; a dialog or another
    // window on top does not, and must not leave a row lit.
    if (programList.isMouseOver(true)) return;
    programList.repaintRow(hoveredRow);
    hoveredRow = -1;
}

void AVDexedPlayView::listBoxItemClicked(int row, const MouseEvent& event) {
    if (!isPositiveAndBelow(row, static_cast<int>(visibleEntries.size()))) return;
    if (!event.mods.isPopupMenu() && event.eventComponent != nullptr
        && event.x >= event.eventComponent->getWidth() - 36) {
        // The row's star toggles the favourite without loading the sound.
        toggleFavoriteKey(visibleEntries[static_cast<size_t>(row)].key);
        return;
    }
    if (!event.mods.isPopupMenu()) {
        requestEntryLoad(row);
        return;
    }
    const auto entry = visibleEntries[static_cast<size_t>(row)];
    auto& language = AVDexedLanguageService::getInstance();
    PopupMenu menu;
    menu.addItem(2, language.text("favorite"), true, favoriteKeys.contains(entry.key),
                 avDrawableFromSvg(BinaryData::AVDexed_FavoriteIcon_svg, BinaryData::AVDexed_FavoriteIcon_svgSize));
    menu.addSeparator();
    menu.addItem(1, language.text("newFolder"));
    StringArray folderIds;
    if (!presetFolders.empty()) {
        PopupMenu assignments;
        int itemId = 100;
        for (const auto& folder : presetFolders) {
            assignments.addItem(itemId++, folder.name, true, folder.presetKeys.contains(entry.key),
                                avDrawableFromSvg(BinaryData::AVDexed_FolderIcon_svg, BinaryData::AVDexed_FolderIcon_svgSize));
            folderIds.add(folder.id);
        }
        menu.addSubMenu(language.text("addToFolder"), assignments);
    }
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    menu.setLookAndFeel(&getLookAndFeel());  // menus never inherit it from their target
    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&programList).withMousePosition(),
                       [safeThis, entry, folderIds](int result) {
        if (safeThis == nullptr || result == 0) return;
        if (result == 1) {
            safeThis->editFolderName({}, {}, entry.key);
        } else if (result == 2) {
            const int index = safeThis->favoriteKeys.indexOf(entry.key);
            if (index >= 0) safeThis->favoriteKeys.remove(index);
            else safeThis->favoriteKeys.addIfNotAlreadyThere(entry.key);
            safeThis->saveFavorites();
            safeThis->rebuildFilter();
            safeThis->refreshLabels();
        } else if (isPositiveAndBelow(result - 100, folderIds.size())) {
            safeThis->togglePresetFolder(folderIds[result - 100], entry.key);
        }
    });
}

void AVDexedPlayView::returnKeyPressed(int row) { requestEntryLoad(row); }

void AVDexedPlayView::rebuildFilter() {
    visibleEntries.clear();
    const auto words = StringArray::fromTokens(searchField.getText().trim(), " ", "\"");
    const auto* customFolder = findFolder(selectedFolderKey);
    for (const auto& entry : libraryEntries) {
        const auto searchable = entry.name + " " + entry.source;
        bool accepted = true;
        for (const auto& word : words)
            if (!searchable.containsIgnoreCase(word)) { accepted = false; break; }
        if (!accepted) continue;
        if (selectedFolderKey == "favorites") accepted = favoriteKeys.contains(entry.key);
        else if (selectedFolderKey == "stock") accepted = entry.isStock;
        else if (selectedFolderKey == "user") accepted = !entry.isStock;
        else if (selectedFolderKey != "all") accepted = customFolder != nullptr && customFolder->presetKeys.contains(entry.key);
        if (accepted) visibleEntries.push_back(entry);
    }
    programList.updateContent();
    browserCount.setText(libraryIndexing && libraryEntries.empty() ? String("...") : String(visibleEntries.size()), dontSendNotification);
    emptyLibraryLabel.setText(AVDexedLanguageService::getInstance().text(libraryIndexing ? "indexingPresets" : "noPresets"), dontSendNotification);
    emptyLibraryLabel.setVisible(visibleEntries.empty());
    selectActivePreset();
}

String AVDexedPlayView::activePresetKey() const {
    if (!processor.hasAVKnownPresetOrigin()) return {};
    const auto id = processor.getAVPresetId();
    if (id.isNotEmpty()) return "user:" + id;
    const auto activeFile = processor.getAVPresetPath();
    if (activeFile.getFullPathName().isNotEmpty()) {
        for (const auto& entry : libraryEntries)
            if (entry.isUser && entry.file == activeFile) return entry.key;
        return "user-path:" + activeFile.getFullPathName();
    }
    if (!processor.currentCart.containsData()) return {};
    const auto& cart = static_cast<const Cartridge&>(processor.currentCart);
    return "bank:" + avStableStateHash(cart.getVoiceSysex() + 6, 4096)
        + ":" + String(processor.getCurrentProgram()).paddedLeft('0', 2);
}

void AVDexedPlayView::selectActivePreset() {
    const auto key = activePresetKey();
    programList.deselectAllRows();
    for (int row = 0; row < static_cast<int>(visibleEntries.size()); ++row)
        if (visibleEntries[static_cast<size_t>(row)].key == key) {
            programList.selectRow(row, dontSendNotification);
            return;
        }
}

void AVDexedPlayView::navigatePreset(int direction) {
    if (visibleEntries.empty()) return;
    const auto key = activePresetKey();
    int row = direction > 0 ? -1 : 0;
    for (int i = 0; i < static_cast<int>(visibleEntries.size()); ++i)
        if (visibleEntries[static_cast<size_t>(i)].key == key) { row = i; break; }
    const int count = static_cast<int>(visibleEntries.size());
    row = (row + direction + count) % count;
    programList.selectRow(row, dontSendNotification);
    programList.scrollToEnsureRowIsOnscreen(row);
    requestEntryLoad(row);
}

void AVDexedPlayView::selectProgram(int row) {
    if (!isPositiveAndBelow(row, static_cast<int>(visibleEntries.size()))) return;
    const auto entry = visibleEntries[static_cast<size_t>(row)];
    if (entry.bank == nullptr || !onProgramSelectionRequested) return;
    const int token = nextRequestToken++;
    requestedEntries.emplace(token, entry);
    if (requestedEntries.size() > 256) requestedEntries.erase(requestedEntries.begin());
    onProgramSelectionRequested(token, entry.isStock);
}

void AVDexedPlayView::commitProgramSelection(int token, bool) {
    const auto requested = requestedEntries.find(token);
    if (requested == requestedEntries.end()) return;
    const auto entry = requested->second;
    requestedEntries.erase(requested);
    if (entry.bank == nullptr || !entry.bank->cartridge.containsData()) return;
    processor.loadCartridge(entry.bank->cartridge);
    processor.activeFileCartridge = entry.bank->file;
    processor.setCurrentProgram(entry.program);
    processor.captureAVPresetBaseline();
    processor.updateHostDisplay();
    selectActivePreset();
    refreshLabels();
}

void AVDexedPlayView::updateBank() {
    loadFavorites();
    const auto activeBank = processor.activeFileCartridge.getFullPathName();
    if (activeBank.isNotEmpty()) knownBankPaths.addIfNotAlreadyThere(activeBank);
    const auto activePreset = processor.getAVPresetPath().getFullPathName();
    if (activePreset.isNotEmpty()) knownPresetPaths.addIfNotAlreadyThere(activePreset);
    PropertiesFile::Options options;
    PropertiesFile preferences(DexedAudioProcessor::dexedAppDir.getChildFile("Dexed.xml"), options);
    auto storedBanks = StringArray::fromLines(preferences.getValue("presetBrowserKnownBanks"));
    auto storedPresets = StringArray::fromLines(preferences.getValue("presetBrowserKnownPresets"));
    storedBanks.addArray(knownBankPaths);
    storedPresets.addArray(knownPresetPaths);
    storedBanks.removeDuplicates(false);
    storedPresets.removeDuplicates(false);
    knownBankPaths = std::move(storedBanks);
    knownPresetPaths = std::move(storedPresets);
    preferences.setValue("presetBrowserKnownBanks", knownBankPaths.joinIntoString("\n"));
    preferences.setValue("presetBrowserKnownPresets", knownPresetPaths.joinIntoString("\n"));
    preferences.setValue("presetBrowserLibraryIndexVersion", 1);
    preferences.save();
    avBrowserPreferenceChanges.sendChangeMessage();
    reindexLibrary();
    refreshLabels();
}

void AVDexedPlayView::reindexLibrary() {
    libraryIndexing = true;
    rebuildFilter();
    if (libraryIndexCancelled != nullptr) libraryIndexCancelled->store(true);
    libraryIndexCancelled = std::make_shared<std::atomic<bool>>(false);
    const auto cancelled = libraryIndexCancelled;
    const auto generation = ++libraryGeneration;
    const auto bankPaths = knownBankPaths;
    const auto presetPaths = knownPresetPaths;
    const auto cartridgeDirectory = DexedAudioProcessor::dexedCartDir;
    const auto userDirectory = DexedAudioProcessor::dexedAppDir.getChildFile("User Presets");
    const auto activeBank = processor.activeFileCartridge;
    const auto activeCartridge = processor.currentCart;
    const auto previousIndex = libraryEntries;
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    libraryIndexPool.addJob([cancelled, generation, bankPaths, presetPaths, cartridgeDirectory,
                             userDirectory, activeBank, activeCartridge, previousIndex, safeThis] {
        std::vector<PresetEntry> entries;
        auto cachedEntries = previousIndex;
        PropertiesFile::Options cacheOptions;
        PropertiesFile cachedPreferences(userDirectory.getParentDirectory().getChildFile("Dexed.xml"), cacheOptions);
        if (auto xml = XmlDocument::parse(cachedPreferences.getValue("presetBrowserLibraryIndex"))) {
            const auto cache = ValueTree::fromXml(*xml);
            if (cache.hasType("AVPresetLibraryIndex") && static_cast<int>(cache.getProperty("version", 0)) == 1)
                for (int i = 0; i < cache.getNumChildren(); ++i) {
                    const auto item = cache.getChild(i);
                    const auto path = item.getProperty("file").toString();
                    if (!item.hasType("NativePreset") || path.isEmpty()) continue;
                    PresetEntry cached;
                    cached.file = File(path);
                    cached.name = item.getProperty("name").toString();
                    cached.source = item.getProperty("source").toString();
                    cached.key = item.getProperty("key").toString();
                    cached.isUser = true;
                    cached.indexedFileSize = static_cast<int64>(item.getProperty("size", 0));
                    cached.indexedModificationTime = static_cast<int64>(item.getProperty("modified", 0));
                    cachedEntries.push_back(std::move(cached));
                }
        }
        StringArray indexedBankHashes;
        const auto addBank = [&entries, &indexedBankHashes](Cartridge cartridge, const File& file,
                                                          const String& name, bool stock) {
            if (!cartridge.containsData()) return;
            auto bank = std::make_shared<AVPresetBank>();
            bank->cartridge = cartridge;
            bank->file = file;
            const auto& bankData = static_cast<const Cartridge&>(bank->cartridge);
            bank->legacyFingerprint = avStableStateHash(bankData.getVoiceSysex(), SYSEX_SIZE);
            bank->fingerprint = avStableStateHash(bankData.getVoiceSysex() + 6, 4096);
            bank->legacyIdentityPrefixes.add("bank:" + bank->legacyFingerprint + ":");
            if (file.getFullPathName().isNotEmpty())
                bank->legacyIdentityPrefixes.add("bank:" + file.getFullPathName().toLowerCase() + ":");
            if (indexedBankHashes.contains(bank->fingerprint)) {
                const auto existing = std::find_if(entries.begin(), entries.end(), [&bank](const PresetEntry& entry) {
                    return entry.bank != nullptr && entry.bank->fingerprint == bank->fingerprint;
                });
                if (existing != entries.end())
                    for (const auto& alias : bank->legacyIdentityPrefixes)
                        existing->bank->legacyIdentityPrefixes.addIfNotAlreadyThere(alias);
                return;
            }
            indexedBankHashes.add(bank->fingerprint);
            StringArray names;
            bank->cartridge.getProgramNames(names);
            for (int i = 0; i < names.size(); ++i)
                entries.push_back({ i, {}, names[i], name,
                    "bank:" + bank->fingerprint + ":" + String(i).paddedLeft('0', 2), false, stock, bank });
        };
        MemoryInputStream archiveData(BinaryData::builtin_pgm_zip, BinaryData::builtin_pgm_zipSize, false);
        ZipFile archive(archiveData);
        for (int i = 0; i < archive.getNumEntries() && !cancelled->load(); ++i) {
            const auto archiveName = archive.getEntry(i)->filename;
            if (!archiveName.endsWithIgnoreCase(".syx")) continue;
            auto stream = std::unique_ptr<InputStream>(archive.createStreamForEntry(i));
            Cartridge cart;
            if (stream != nullptr && cart.load(*stream) <= 1)
                addBank(cart, {}, archiveName.fromLastOccurrenceOf("/", false, false).upToLastOccurrenceOf(".", false, false), true);
        }
        Array<File> files;
        cartridgeDirectory.findChildFiles(files, File::findFiles, true, "*.syx;*.SYX");
        for (const auto& path : bankPaths) files.addIfNotAlreadyThere(File(path));
        if (activeBank.getFullPathName().isNotEmpty()) files.addIfNotAlreadyThere(activeBank);
        if (activeCartridge.containsData())
            addBank(activeCartridge, activeBank, activeBank.getFileNameWithoutExtension(), false);
        for (const auto& file : files) {
            if (cancelled->load()) return;
            Cartridge cart;
            if (file.existsAsFile() && file.getSize() > 0 && file.getSize() < 65536 && cart.load(file) <= 1)
                addBank(cart, file, file.getFileNameWithoutExtension(), false);
        }
        files.clear();
        userDirectory.findChildFiles(files, File::findFiles, true, "*.dexedpreset");
        for (const auto& path : presetPaths) files.addIfNotAlreadyThere(File(path));
        for (const auto& file : files) {
            if (cancelled->load()) return;
            const auto fileSize = file.getSize();
            const auto modified = file.getLastModificationTime().toMilliseconds();
            const auto cached = std::find_if(cachedEntries.begin(), cachedEntries.end(), [&file, fileSize, modified](const PresetEntry& entry) {
                return entry.isUser && entry.file == file && entry.indexedFileSize == fileSize
                    && entry.indexedModificationTime == modified && entry.key.isNotEmpty();
            });
            if (file.existsAsFile() && cached != cachedEntries.end()) {
                entries.push_back(*cached);
                continue;
            }
            MemoryBlock contents;
            if (!file.existsAsFile() || file.getSize() <= 0 || file.getSize() > 32 * 1024 * 1024
                || !file.loadFileAsData(contents)) continue;
            auto xml = AudioProcessor::getXmlFromBinary(contents.getData(), static_cast<int>(contents.getSize()));
            if (xml == nullptr) continue;
            const auto tree = ValueTree::fromXml(*xml);
            const auto stableId = tree.getProperty("libraryPresetId").toString().trim();
            const auto session = tree.getChildWithName("session");
            auto name = session.getProperty("avPresetName").toString();
            if (name.isEmpty()) name = file.getFileNameWithoutExtension();
            const auto key = stableId.isNotEmpty() ? "user:" + stableId
                : "user-content:" + avStableStateHash(contents.getData(), contents.getSize());
            entries.push_back({ -1, file, name, file.getParentDirectory().getFileName(), key, true, false, {}, fileSize, modified });
        }
        if (cancelled->load()) return;
        MessageManager::callAsync([safeThis, generation, entries = std::move(entries)]() mutable {
            if (safeThis != nullptr) safeThis->publishLibrary(std::move(entries), generation);
        });
    });
}

void AVDexedPlayView::publishLibrary(std::vector<PresetEntry> entries, unsigned generation) {
    if (generation != libraryGeneration) return;
    libraryIndexing = false;
    libraryEntries = std::move(entries);
    for (const auto& entry : libraryEntries) {
        if (entry.isUser) migratePresetKey("user:" + entry.file.getFullPathName().toLowerCase(), entry.key);
        else if (entry.bank != nullptr && entry.bank->file.getFullPathName().isNotEmpty())
            migratePresetKey("bank:" + entry.bank->file.getFullPathName().toLowerCase()
                + ":" + String(entry.program).paddedLeft('0', 2), entry.key);
        if (entry.bank != nullptr)
            for (const auto& alias : entry.bank->legacyIdentityPrefixes)
                migratePresetKey(alias + String(entry.program).paddedLeft('0', 2), entry.key);
        if (entry.isStock && entry.source == "Dexed_01")
            migratePresetKey("stock:" + String(entry.program).paddedLeft('0', 2), entry.key);
    }
    ValueTree index("AVPresetLibraryIndex");
    index.setProperty("version", 1, nullptr);
    for (const auto& entry : libraryEntries) {
        if (!entry.isUser) continue;
        ValueTree item("NativePreset");
        item.setProperty("file", entry.file.getFullPathName(), nullptr);
        item.setProperty("name", entry.name, nullptr);
        item.setProperty("source", entry.source, nullptr);
        item.setProperty("key", entry.key, nullptr);
        item.setProperty("size", entry.indexedFileSize, nullptr);
        item.setProperty("modified", entry.indexedModificationTime, nullptr);
        index.addChild(item, -1, nullptr);
    }
    PropertiesFile::Options options;
    PropertiesFile preferences(DexedAudioProcessor::dexedAppDir.getChildFile("Dexed.xml"), options);
    preferences.setValue("presetBrowserLibraryIndex", index.toXmlString());
    preferences.save();
    rebuildFilter();
    refreshLabels();
}

void AVDexedPlayView::refreshLabels() {
    auto& language = AVDexedLanguageService::getInstance();
    const auto presetName = processor.getAVPresetName();
    const auto soundName = presetName.isNotEmpty() ? presetName : language.text("presetUnknown");
    const auto activePreset = processor.getAVPresetPath();
    const bool nativePreset = activePreset.getFullPathName().isNotEmpty();
    // Like GOZO (preset above, instrument in the centre): the top bar names the
    // sound, the centre names the collection it comes from.
    String collection, detail;
    const auto key = activePresetKey();
    const auto entry = std::find_if(libraryEntries.begin(), libraryEntries.end(),
                                    [&key](const PresetEntry& candidate) { return candidate.key == key; });
    if (nativePreset) {
        collection = language.text("userPresets");
        detail = activePreset.getFileNameWithoutExtension();
    } else {
        collection = entry != libraryEntries.end() && entry->source.isNotEmpty() ? entry->source
                   : processor.hasExternalCartridgeContext() ? processor.activeFileCartridge.getFileNameWithoutExtension()
                   : language.text("factoryBank");
        detail = language.format("programOfBank", {{"number", String(processor.getCurrentProgram() + 1)}});
    }
    if ((nativePreset && !activePreset.existsAsFile())
        || (!nativePreset && processor.hasExternalCartridgeContext() && !processor.activeFileCartridge.existsAsFile()))
        detail = language.text("sourceUnavailable") + String::fromUTF8(" · ") + detail;
    if (!processor.hasAVKnownPresetOrigin()) {
        collection = language.text("sourceUnknown");
        detail = {};
    }
    activeName.setText(collection, dontSendNotification);
    activeName.setTooltip(collection);
    sourceName.setText(detail, dontSendNotification);
    sourceName.setTooltip(nativePreset ? activePreset.getFullPathName() : processor.activeFileCartridge.getFullPathName());
    modifiedCurrent = processor.isAVPresetModified();
    modifiedLabel.setText({}, dontSendNotification);
    presetNavigator.setPreset(soundName, {}, modifiedCurrent,
                              favoriteKeys.contains(key), processor.hasAVKnownPresetOrigin());
    if (heroAlgorithm != nullptr) {
        const auto* feedback = processor.parameters.getRawParameterValue(IDs::feedback.name);
        algorithmCaption.setText(language.format("algorithmCaption", {{"number", String(heroAlgorithm->getAlgorithm())},
                                                                     {"feedback", String(feedback != nullptr ? roundToInt(feedback->load()) : 0)}}),
                                 dontSendNotification);
    }
    const std::array<String, 3> bypassIds { IDs::avReverbBypass.name, IDs::avDelayBypass.name, IDs::avChorusBypass.name };
    const std::array<Slider*, 3> amounts { &reverbSlider, &delaySlider, &chorusSlider };
    for (size_t i = 0; i < bypassIds.size() && knobCaptions[i + 3] != nullptr; ++i) {
        const auto* parameter = processor.parameters.getRawParameterValue(bypassIds[i]);
        const bool bypassed = parameter != nullptr && parameter->load() >= 0.5f;
        const auto caption = knobCaptions[i + 3]->getTitle();
        knobCaptions[i + 3]->setButtonText(bypassed ? caption + String::fromUTF8(" · ") + language.text("bypass") : caption);
        knobCaptions[i + 3]->setAlpha(bypassed ? 0.55f : 1.0f);
        amounts[i]->setAlpha(bypassed ? 0.4f : 1.0f);
        amounts[i]->setEnabled(!bypassed);
        amounts[i]->setTooltip(bypassed ? language.text("bypass") : caption + String::fromUTF8(" · ") + language.text("amount"));
    }
}

File AVDexedPlayView::getActiveUserPreset() const { return processor.getAVPresetPath(); }

void AVDexedPlayView::setActiveUserPreset(const File& file) {
    if (file.getFullPathName().isEmpty()) {
        processor.setAVPresetContext({}, {}, {});
    } else if (processor.getAVPresetPath() != file) {
        const auto key = fingerprintFile(file);
        processor.setAVPresetContext(file.getFileNameWithoutExtension(), key.startsWith("user:") ? key.substring(5) : String(), file);
    }
    captureCurrentAsBaseline();
    updateBank();
}

void AVDexedPlayView::captureCurrentAsBaseline() {
    processor.captureAVPresetBaseline();
    refreshLabels();
}

String AVDexedPlayView::fingerprintProgram(int program, bool) const {
    if (!isPositiveAndBelow(program, processor.getNumPrograms()) || !processor.currentCart.containsData()) return {};
    const auto& cart = static_cast<const Cartridge&>(processor.currentCart);
    return "bank:" + avStableStateHash(cart.getVoiceSysex() + 6, 4096)
        + ":" + String(program).paddedLeft('0', 2);
}

String AVDexedPlayView::fingerprintFile(const File& file) const {
    for (const auto& entry : libraryEntries)
        if (entry.isUser && entry.file == file) return entry.key;
    if (!file.existsAsFile() || file.getSize() <= 0 || file.getSize() > 32 * 1024 * 1024) return {};
    MemoryBlock contents;
    if (!file.loadFileAsData(contents)) return {};
    if (auto xml = AudioProcessor::getXmlFromBinary(contents.getData(), static_cast<int>(contents.getSize()))) {
        const auto id = ValueTree::fromXml(*xml).getProperty("libraryPresetId").toString().trim();
        if (id.isNotEmpty()) return "user:" + id;
    }
    return "user-content:" + avStableStateHash(contents.getData(), contents.getSize());
}

void AVDexedPlayView::migratePresetKey(const String& oldKey, const String& newKey) {
    if (oldKey.isEmpty() || newKey.isEmpty() || oldKey == newKey)
        return;

    bool changed = false;
    const auto favoriteIndex = favoriteKeys.indexOf(oldKey);
    if (favoriteIndex >= 0) {
        favoriteKeys.remove(favoriteIndex);
        favoriteKeys.addIfNotAlreadyThere(newKey);
        changed = true;
    }
    for (auto& folder : presetFolders) {
        const int index = folder.presetKeys.indexOf(oldKey);
        if (index >= 0) {
            folder.presetKeys.remove(index);
            folder.presetKeys.addIfNotAlreadyThere(newKey);
            changed = true;
        }
    }
    if (changed) {
        saveFavorites();
        saveFolderCatalog();
    }
}

void AVDexedPlayView::carryAVBankMetadata(const Cartridge& previousBank, const Cartridge& storedBank) {
    if (!previousBank.containsData() || !storedBank.containsData()) return;
    const auto oldFingerprint = avStableStateHash(previousBank.getVoiceSysex() + 6, 4096);
    const auto newFingerprint = avStableStateHash(storedBank.getVoiceSysex() + 6, 4096);
    if (oldFingerprint == newFingerprint) return;
    bool changed = false;
    for (int slot = 0; slot < 32; ++slot) {
        const auto suffix = ":" + String(slot).paddedLeft('0', 2);
        const auto previousKey = "bank:" + oldFingerprint + suffix;
        const auto storedKey = "bank:" + newFingerprint + suffix;
        if (favoriteKeys.contains(previousKey) && !favoriteKeys.contains(storedKey)) {
            favoriteKeys.add(storedKey);
            changed = true;
        }
        for (auto& folder : presetFolders)
            if (folder.presetKeys.contains(previousKey) && !folder.presetKeys.contains(storedKey)) {
                folder.presetKeys.add(storedKey);
                changed = true;
            }
    }
    // Keep the source metadata too: a stored bank may be a copy, and the
    // embedded stock bank or an older file can still legitimately exist.
    if (changed) {
        saveFavorites();
        saveFolderCatalog();
    }
}

AVDexedPlayView::PresetFolder* AVDexedPlayView::findFolder(const String& id) {
    for (auto& folder : presetFolders)
        if (folder.id == id)
            return &folder;
    return nullptr;
}

const AVDexedPlayView::PresetFolder* AVDexedPlayView::findFolder(const String& id) const {
    for (const auto& folder : presetFolders)
        if (folder.id == id)
            return &folder;
    return nullptr;
}

void AVDexedPlayView::loadFolderCatalog() {
    PropertiesFile::Options options;
    PropertiesFile preferences(DexedAudioProcessor::dexedAppDir.getChildFile("Dexed.xml"), options);
    presetFolders.clear();
    if (auto xml = XmlDocument::parse(preferences.getValue("presetBrowserFolders"))) {
        auto root = ValueTree::fromXml(*xml);
        if (root.hasType("PresetFolders")) {
            for (int i = 0; i < root.getNumChildren(); ++i) {
                auto folderTree = root.getChild(i);
                if (!folderTree.hasType("Folder"))
                    continue;
                PresetFolder folder;
                folder.id = folderTree.getProperty("id").toString();
                folder.name = folderTree.getProperty("name").toString().trim();
                for (int j = 0; j < folderTree.getNumChildren(); ++j) {
                    auto keyTree = folderTree.getChild(j);
                    if (keyTree.hasType("PresetKey"))
                        folder.presetKeys.addIfNotAlreadyThere(keyTree.getProperty("value").toString());
                }
                if (folder.id.isNotEmpty() && folder.name.isNotEmpty())
                    presetFolders.push_back(std::move(folder));
            }
        }
    }
    knownBankPaths = StringArray::fromLines(preferences.getValue("presetBrowserKnownBanks"));
    knownPresetPaths = StringArray::fromLines(preferences.getValue("presetBrowserKnownPresets"));
    selectedFolderKey = preferences.getValue("presetBrowserSelectedFolder", "all");
    if (selectedFolderKey != "all" && selectedFolderKey != "favorites" && selectedFolderKey != "stock"
        && selectedFolderKey != "user" && findFolder(selectedFolderKey) == nullptr)
        selectedFolderKey = "all";
}

void AVDexedPlayView::saveFolderCatalog() {
    ValueTree root("PresetFolders");
    for (const auto& folder : presetFolders) {
        ValueTree folderTree("Folder");
        folderTree.setProperty("id", folder.id, nullptr);
        folderTree.setProperty("name", folder.name, nullptr);
        for (const auto& key : folder.presetKeys) {
            ValueTree keyTree("PresetKey");
            keyTree.setProperty("value", key, nullptr);
            folderTree.addChild(keyTree, -1, nullptr);
        }
        root.addChild(folderTree, -1, nullptr);
    }

    PropertiesFile::Options options;
    PropertiesFile preferences(DexedAudioProcessor::dexedAppDir.getChildFile("Dexed.xml"), options);
    preferences.setValue("presetBrowserFolders", root.toXmlString());
    preferences.setValue("presetBrowserLibraryIndexVersion", 1);
    preferences.setValue("presetBrowserKnownBanks", knownBankPaths.joinIntoString("\n"));
    preferences.setValue("presetBrowserKnownPresets", knownPresetPaths.joinIntoString("\n"));
    preferences.setValue("presetBrowserSelectedFolder", selectedFolderKey);
    preferences.save();
    avBrowserPreferenceChanges.sendChangeMessage();
}

void AVDexedPlayView::rebuildFolderPicker() {
    auto& language = AVDexedLanguageService::getInstance();
    String name = language.text("allPresets");
    if (selectedFolderKey == "favorites")
        name = language.text("favorites");
    else if (selectedFolderKey == "stock")
        name = "Dexed stock presets";
    else if (selectedFolderKey == "user")
        name = language.text("userPresets");
    else if (const auto* folder = findFolder(selectedFolderKey))
        name = folder->name;
    else
        selectedFolderKey = "all";
    folderPickerButton.setSelection(name, selectedFolderKey == "favorites");
}

void AVDexedPlayView::setSelectedFolder(const String& folderKey) {
    if (folderKey != "all" && folderKey != "favorites" && folderKey != "stock" && folderKey != "user"
        && findFolder(folderKey) == nullptr)
        return;
    selectedFolderKey = folderKey;
    saveFolderCatalog();
    rebuildFolderPicker();
    rebuildFilter();
}

void AVDexedPlayView::showFolderPicker() {
    auto& language = AVDexedLanguageService::getInstance();
    PopupMenu menu;
    const auto addChoice = [this, &menu](int id, const String& key, const String& label, bool favorite) {
        menu.addItem(id, label, true, selectedFolderKey == key,
                     avDrawableFromSvg(favorite ? BinaryData::AVDexed_FavoriteIcon_svg : BinaryData::AVDexed_FolderIcon_svg,
                                     favorite ? BinaryData::AVDexed_FavoriteIcon_svgSize : BinaryData::AVDexed_FolderIcon_svgSize));
    };
    addChoice(1, "all", language.text("allPresets"), false);
    addChoice(2, "favorites", language.text("favorites"), true);
    addChoice(3, "stock", "Dexed stock presets", false);
    addChoice(4, "user", language.text("userPresets"), false);
    if (!presetFolders.empty()) {
        menu.addSeparator();
        int id = 100;
        for (const auto& folder : presetFolders)
            addChoice(id++, folder.id, folder.name, false);
    }

    StringArray folderIds;
    for (const auto& folder : presetFolders)
        folderIds.add(folder.id);
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    menu.setLookAndFeel(&getLookAndFeel());  // menus never inherit it from their target
    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&folderPickerButton),
                       [safeThis, folderIds](int result) {
        if (safeThis == nullptr || result == 0)
            return;
        if (result == 1) safeThis->setSelectedFolder("all");
        else if (result == 2) safeThis->setSelectedFolder("favorites");
        else if (result == 3) safeThis->setSelectedFolder("stock");
        else if (result == 4) safeThis->setSelectedFolder("user");
        else if (isPositiveAndBelow(result - 100, folderIds.size()))
            safeThis->setSelectedFolder(folderIds[result - 100]);
    });
}

void AVDexedPlayView::showFolderActions() {
    auto& language = AVDexedLanguageService::getInstance();
    const bool customSelected = findFolder(selectedFolderKey) != nullptr;
    PopupMenu menu;
    menu.addItem(1, language.text("newFolder"));
    menu.addItem(4, language.text("reindexLibrary"));
    menu.addSeparator();
    menu.addItem(2, language.text("renameFolder"), customSelected);
    menu.addItem(3, language.text("deleteFolder"), customSelected);
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    const auto id = selectedFolderKey;
    menu.setLookAndFeel(&getLookAndFeel());  // menus never inherit it from their target
    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&folderActionsButton),
                       [safeThis, id](int result) {
        if (safeThis == nullptr)
            return;
        if (result == 1) safeThis->editFolderName({});
        else if (result == 4) safeThis->updateBank();
        else if (result == 2) {
            if (const auto* folder = safeThis->findFolder(id))
                safeThis->editFolderName(id, folder->name);
        } else if (result == 3) {
            safeThis->deleteSelectedFolder();
        }
    });
}

void AVDexedPlayView::editFolderName(const String& id, const String& initialName, const String& presetToAssign) {
    auto& language = AVDexedLanguageService::getInstance();
    auto dialog = std::make_shared<AVFolderNameDialog>(id.isEmpty() ? language.text("newFolder") : language.text("renameFolder"),
                                                    language.text("folderNamePrompt"));
    dialog->addTextEditor("folderName", initialName, language.text("folderName"));
    dialog->addButton(language.text("save"), 1);
    dialog->addButton(language.text("cancel"), 0);
    dialog->setSize(460, 196);
    dialog->centreAroundComponent(this, 460, 196);
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    dialog->enterModalState(true, ModalCallbackFunction::create([safeThis, dialog, id, presetToAssign](int result) {
        if (safeThis != nullptr && result == 1)
            safeThis->applyFolderName(id, dialog->getTextEditorContents("folderName"), presetToAssign);
    }), false);
}

void AVDexedPlayView::applyFolderName(const String& id, const String& requestedName, const String& presetToAssign) {
    const auto name = requestedName.trim().substring(0, 48);
    if (name.isEmpty())
        return;
    for (const auto& folder : presetFolders)
        if (folder.id != id && folder.name.equalsIgnoreCase(name)) {
            AVShowMessage(AlertWindow::WarningIcon, AVDexedLanguageService::getInstance().text("folderName"),
                          AVDexedLanguageService::getInstance().text("folderExists"), this);
            return;
        }
    if (id.isEmpty()) {
        PresetFolder folder;
        folder.id = Uuid().toString();
        folder.name = name;
        if (presetToAssign.isNotEmpty()) folder.presetKeys.add(presetToAssign);
        presetFolders.push_back(std::move(folder));
        selectedFolderKey = presetFolders.back().id;
    } else if (auto* folder = findFolder(id)) {
        folder->name = name;
        selectedFolderKey = id;
    }
    saveFolderCatalog();
    rebuildFolderPicker();
    rebuildFilter();
}

void AVDexedPlayView::deleteSelectedFolder() {
    const auto id = selectedFolderKey;
    const auto* folder = findFolder(id);
    if (folder == nullptr)
        return;
    const auto folderName = folder->name;
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    AlertWindow::showOkCancelBox(AlertWindow::QuestionIcon,
                                 AVDexedLanguageService::getInstance().text("deleteFolder"),
                                 AVDexedLanguageService::getInstance().text("deleteFolderQuestion") + "\n\n" + folderName,
                                 AVDexedLanguageService::getInstance().text("deleteFolder"),
                                 AVDexedLanguageService::getInstance().text("cancel"), this,
                                 ModalCallbackFunction::create([safeThis, id](int result) {
        if (safeThis == nullptr || result == 0)
            return;
        auto& folders = safeThis->presetFolders;
        folders.erase(std::remove_if(folders.begin(), folders.end(), [&id](const PresetFolder& folder) {
            return folder.id == id;
        }), folders.end());
        safeThis->selectedFolderKey = "all";
        safeThis->saveFolderCatalog();
        safeThis->rebuildFolderPicker();
        safeThis->rebuildFilter();
    }));
}

void AVDexedPlayView::togglePresetFolder(const String& folderId, const String& presetKey) {
    if (auto* folder = findFolder(folderId)) {
        const int index = folder->presetKeys.indexOf(presetKey);
        if (index >= 0)
            folder->presetKeys.remove(index);
        else
            folder->presetKeys.add(presetKey);
        saveFolderCatalog();
        rebuildFilter();
    }
}

void AVDexedPlayView::toggleFavorite() {
    toggleFavoriteKey(activePresetKey());
}

void AVDexedPlayView::toggleFavoriteKey(const String& key) {
    if (key.isEmpty()) return;
    const int index = favoriteKeys.indexOf(key);
    if (index >= 0) favoriteKeys.remove(index);
    else favoriteKeys.addIfNotAlreadyThere(key);
    saveFavorites();
    rebuildFilter();
    refreshLabels();
}

void AVDexedPlayView::setOperatorLevels(const std::array<float, 6>& levels) {
    if (heroAlgorithm != nullptr && isShowing())
        heroAlgorithm->setLevels(levels);
}

void AVDexedPlayView::showValueTip(Slider& slider) {
    valueTip.show(slider, slider.getTextFromValue(slider.getValue()));
}

void AVDexedPlayView::loadFavorites() {
    PropertiesFile::Options options;
    PropertiesFile preferences(DexedAudioProcessor::dexedAppDir.getChildFile("Dexed.xml"), options);
    favoriteKeys = StringArray::fromLines(preferences.getValue("favoritePresetIds"));
}

void AVDexedPlayView::saveFavorites() {
    PropertiesFile::Options options;
    PropertiesFile preferences(DexedAudioProcessor::dexedAppDir.getChildFile("Dexed.xml"), options);
    preferences.setValue("favoritePresetIds", favoriteKeys.joinIntoString("\n"));
    preferences.save();
    avBrowserPreferenceChanges.sendChangeMessage();
}
