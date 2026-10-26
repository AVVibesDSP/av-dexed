// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedPlayView.h"
#include "AVDexedKnob.h"
#include "AVDexedFade.h"
#include "../parameter/Model.h"
#include "../JuceLibraryCode/BinaryData.h"
#include "util/ContextMenuAdapter.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <set>

using namespace juce;

namespace {
enum class AVEffectKind { reverb, delay, chorus };
// Favourites changed: every Play view of this process re-reads them.
ChangeBroadcaster avBrowserPreferenceChanges;
// A Play view changed the library on disk: the others compare their signature.
ChangeBroadcaster avLibraryChanges;

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
    return AVDexedLibrary::stableHash(data, size);   // the 0.1.0 identity: favourites depend on it
}

String avProgramSuffix(int program) { return ":" + String(program).paddedLeft('0', 2); }

String avEllipsis(const String& text) { return text + String::fromUTF8("…"); }

/** Cmd, Ctrl or Alt: an arrow with one of them belongs to the editor's shortcuts. */
bool avHasShortcutModifier(const KeyPress& key) {
    const auto mods = key.getModifiers();
    return mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown();
}

/** Up (-1) or Down (+1), Shift allowed: the arrows browse the sounds. With
    Cmd, Ctrl or Alt they are left to the editor's shortcuts (0). */
int avSoundStepOf(const KeyPress& key) {
    if (avHasShortcutModifier(key)) return 0;
    return key.isKeyCode(KeyPress::upKey) ? -1 : key.isKeyCode(KeyPress::downKey) ? 1 : 0;
}

/** Left or Right with no Cmd, Ctrl or Alt: nothing in the Play view moves
    sideways, so they are swallowed (left to the editor they would light the
    focus rings and do nothing else). */
bool avIsSidewaysArrow(const KeyPress& key) {
    return (key.isKeyCode(KeyPress::leftKey) || key.isKeyCode(KeyPress::rightKey)) && ! avHasShortcutModifier(key);
}

/** On a Play knob with focus, Up/Down step through the sounds instead of
    nudging the knob (a knob takes every arrow, so the editor never saw them).
    Left/Right still nudge it, as a focused knob does. */
class AVKnobSoundKeys final : public KeyListener {
public:
    explicit AVKnobSoundKeys(std::function<void(int)> stepTo) : step(std::move(stepTo)) {}
    bool keyPressed(const KeyPress& key, Component*) override {
        const int direction = avSoundStepOf(key);
        if (direction == 0) return false;
        if (step) step(direction);
        return true;
    }

private:
    std::function<void(int)> step;
};

/** One line that fits `width`: the font may shrink down to `minScale`, then
    the text is cut with "…". AVDexedTheme::drawText alone shrinks long text
    to 55%, which left the bank names beside the sounds unreadable. */
std::pair<Font, String> avFitLine(const String& text, Font font, float width, float minScale) {
    // A pixel of slack: Graphics::drawText measures on its own and would cut again.
    const float room = width - 1.0f;
    const float natural = GlyphArrangement::getStringWidth(font, text);
    if (room <= 0.0f || natural <= room) return { font, text };
    font = font.withHeight(font.getHeight() * jmax(minScale, room / natural));
    if (GlyphArrangement::getStringWidth(font, text) <= room) return { font, text };
    const auto ellipsis = String::fromUTF8("…");
    // The longest start that fits with the ellipsis (binary search: rows repaint often).
    int fits = 0, tooLong = text.length();
    while (tooLong - fits > 1) {
        const int middle = (fits + tooLong) / 2;
        if (GlyphArrangement::getStringWidth(font, text.substring(0, middle).trimEnd() + ellipsis) <= room) fits = middle;
        else tooLong = middle;
    }
    return { font, fits > 0 ? text.substring(0, fits).trimEnd() + ellipsis : ellipsis };
}

/** Re-reads the preferences, applies `change` and saves, holding the
    preferences lock the whole time: another AV Dexed (another DAW, the
    standalone) cannot write between the read and the write. */
void avEditPreferences(const std::function<void(PropertiesFile&)>& change) {
    auto& lock = AVDexedLibrary::preferencesLock();
    const bool held = lock.enter(3000);
    {
        PropertiesFile preferences(DexedAudioProcessor::getPreferencesFile(), AVDexedLibrary::preferencesOptions());
        change(preferences);
        preferences.saveIfNeeded();
    }
    if (held) lock.exit();
}

/** Folder paths ("Pianos/Rhodes") compare like the disk does. */
bool avSameFolder(const String& a, const String& b) {
    return File::areFileNamesCaseSensitive() ? a == b : a.equalsIgnoreCase(b);
}

/** A folder shows its own sounds and those of its subfolders. */
bool avInFolder(const String& entryFolder, const String& folder) {
    return folder.isNotEmpty()
        && (avSameFolder(entryFolder, folder)
            || (entryFolder.length() > folder.length() && entryFolder[folder.length()] == '/'
                && avSameFolder(entryFolder.substring(0, folder.length()), folder)));
}

String avRelativeFolder(const File& folder, const File& root) {
    return folder.getRelativePathFrom(root).replaceCharacter('\\', '/');
}

/** A move or removal a Play view made in the library (a folder or a sound
    renamed or deleted; a removal moves into .deleted). Every Play view of this
    process applies it to its own processor before it re-reads the library, so
    a second AV Dexed in the same DAW follows the exact file instead of
    guessing it by content (an identical copy elsewhere is another file).
    `presetName` is a renamed preset's new name. Message thread only. */
struct AVLibraryMove {
    File from, to;
    String presetName;
};

std::vector<AVLibraryMove>& avLibraryMoves() {
    static std::vector<AVLibraryMove> moves;
    return moves;
}

/** How many of those moves each processor has followed, kept while its editor
    is closed: the next Play view built for it carries on from there. */
std::map<const DexedAudioProcessor*, size_t>& avFollowedLibraryMoves() {
    static std::map<const DexedAudioProcessor*, size_t> followed;
    return followed;
}

/** The library copy a file that is gone (or lives outside the library) most
    likely is: the only identical copy, or else the one that keeps its file
    name and more of its folders (read upward from the file) than any other
    copy. File() when that is not clear: two copies that match as well, or a
    copy with another name that shares more of its folders (the file renamed
    in place, maybe). "Guardar" then goes to "Guardar como…" instead of
    writing into a copy the user never chose. */
File avLikelyCopy(const File& gone, const Array<File>& copies, const File& root) {
    if (copies.size() == 1) return copies.getFirst();
    File best;
    int bestFolders = -1, otherFolders = -1;
    bool tie = false;
    for (const auto& copy : copies) {
        int folders = 0;
        for (auto a = gone.getParentDirectory(), b = copy.getParentDirectory();
             b.isAChildOf(root) && avSameFolder(a.getFileName(), b.getFileName());
             a = a.getParentDirectory(), b = b.getParentDirectory())
            ++folders;
        // A sound file renamed from .dexedpreset to .avdexpreset is still "the same name".
        const bool sameName = AVDexedLibrary::isPresetFile(copy) && AVDexedLibrary::isPresetFile(gone)
                                  ? avSameFolder(copy.getFileNameWithoutExtension(), gone.getFileNameWithoutExtension())
                                  : avSameFolder(copy.getFileName(), gone.getFileName());
        if (! sameName) otherFolders = jmax(otherFolders, folders);
        else if (folders == bestFolders) tie = true;
        else if (folders > bestFolders) { best = copy; bestFolders = folders; tie = false; }
    }
    return tie || bestFolders < otherFolders ? File() : best;
}

/** "Papelera" / "Trash"; Windows calls it the Recycle Bin. */
String avTrashName() {
   #if JUCE_WINDOWS
    return AVDexedLanguageService::getInstance().text("recycleBinName");
   #else
    return AVDexedLanguageService::getInstance().text("trashName");
   #endif
}

String avRevealText() {
   #if JUCE_MAC
    return AVDexedLanguageService::getInstance().text("showInFinder");
   #elif JUCE_WINDOWS
    return AVDexedLanguageService::getInstance().text("showInExplorer");
   #else
    return AVDexedLanguageService::getInstance().text("showInFiles");
   #endif
}

/** The name a .dexedpreset shows lives inside it (session avPresetName): a
    rename writes it there too, keeping its id (and so its favourite). */
bool avWriteNativePresetName(const File& file, const String& name) {
    MemoryBlock data;
    if (! file.existsAsFile() || file.getSize() > 32 * 1024 * 1024 || ! file.loadFileAsData(data)) return false;
    auto xml = AudioProcessor::getXmlFromBinary(data.getData(), static_cast<int>(data.getSize()));
    auto* session = xml != nullptr ? xml->getChildByName("session") : nullptr;
    if (session == nullptr) return false;
    session->setAttribute("avPresetName", name);
    MemoryBlock updated;
    AudioProcessor::copyXmlToBinary(*xml, updated);
    const AVDexedLibrary::LibraryScope lock(4000);
    if (! lock.held) return false;
    // Moved or removed meanwhile (another AV Dexed, Finder): not brought back.
    return file.existsAsFile() && AVDexedLibrary::writeAtomically(file, updated);
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
                AVDexedDim::setAlpha(row->label, enabled ? 1.0f : 0.5f);
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
                    const Image& source, std::function<void()> close)
        : AVParent(&parent), AVClose(std::move(close)), AVPanel(parameters, kind),
          AVCloseButton("AVCloseEffect", DrawableButton::ImageFitted) {
        AVBackdrop = AVDexedTheme::blurredBackdrop(source);
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

AVDexedImportButton::AVDexedImportButton() : Button("AVImportBank") {
    setWantsKeyboardFocus(true);
    plusIcon = avDrawableFromSvg(BinaryData::AVDexed_PlusIcon_svg, BinaryData::AVDexed_PlusIcon_svgSize);
}

void AVDexedImportButton::paintButton(Graphics& g, bool isMouseOverButton, bool isButtonDown) {
    const auto bounds = getLocalBounds().toFloat().reduced(0.5f);
    const bool lit = isEnabled() && (isMouseOverButton || isButtonDown);
    AVDexedTheme::fillBox(g, bounds, bounds.getHeight() * 0.5f, lit);
    if (AVDexedTheme::showsFocus(*this)) {
        g.setColour(AVDexedTheme::oceanHighlight);
        g.drawRoundedRectangle(bounds, bounds.getHeight() * 0.5f, 1.2f);
    }
    // "+" and label centred together.
    const auto font = AVDexedTheme::font(14.0f, "Medium");
    const float iconSize = 13.0f, gap = 9.0f;
    const float textWidth = GlyphArrangement::getStringWidth(font, getButtonText());
    const float total = iconSize + gap + textWidth;
    const float x = bounds.getCentreX() - total * 0.5f;
    // The "+" is an action icon, ocean like the folder icon above it: a little
    // dimmer at rest, full on hover (GOZO).
    if (plusIcon != nullptr)
        plusIcon->drawWithin(g, { x, bounds.getCentreY() - iconSize * 0.5f, iconSize, iconSize }, RectanglePlacement::centred,
                             ! isEnabled() ? 0.42f : lit ? 1.0f : 0.78f);
    const auto textArea = Rectangle<float>(x + iconSize + gap, bounds.getY(), textWidth + 4.0f, bounds.getHeight());
    if (lit) AVDexedTheme::drawOceanText(g, getButtonText(), font, textArea, Justification::centredLeft);
    else AVDexedTheme::drawGreyText(g, getButtonText(), font, textArea, Justification::centredLeft, isEnabled() ? 1.0f : 0.42f);
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

String AVDexedActionIconButton::getTooltip() {
    return AVTooltipSource ? AVTooltipSource() : TextButton::getTooltip();
}

void AVDexedActionIconButton::mouseDown(const MouseEvent& event) {
    // The whole gesture belongs to the menu: no press look, no click on release.
    AVPopupGesture = onPopupMenu != nullptr && event.mods.isPopupMenu();
    if (! AVPopupGesture) {
        TextButton::mouseDown(event);
        return;
    }
    if (isEnabled()) onPopupMenu();
}

void AVDexedActionIconButton::mouseDrag(const MouseEvent& event) {
    if (! AVPopupGesture) TextButton::mouseDrag(event);
}

void AVDexedActionIconButton::mouseUp(const MouseEvent& event) {
    if (! AVPopupGesture) TextButton::mouseUp(event);
    AVPopupGesture = false;
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
    // Right-click on an arrow: MIDI learn for it (the Play view's menu).
    previous.onPopupMenu = [this] { if (onStepMenu) onStepMenu(0); };
    next.onPopupMenu = [this] { if (onStepMenu) onStepMenu(1); };
    // Read whenever the help shows, so a CC learned or removed shows at once.
    previous.AVTooltipSource = [this] { return stepHelp(0); };
    next.AVTooltipSource = [this] { return stepHelp(1); };
    applyLanguage();
}

AVDexedPresetNavigator::~AVDexedPresetNavigator() = default;

String AVDexedPresetNavigator::stepHelp(int step) const {
    auto& language = AVDexedLanguageService::getInstance();
    const auto base = language.text(step == 0 ? "previousSound" : "nextSound");
    const int cc = AVStepController ? AVStepController(step) : -1;
    return cc < 0 ? base : language.format("soundStepCC", {{"sound", base}, {"cc", String(cc)}});
}

void AVDexedPresetNavigator::applyLanguage() {
    auto& language = AVDexedLanguageService::getInstance();
    previous.setTitle(language.text("previousSound"));
    previous.setTooltip(language.text("previousSound"));
    next.setTitle(language.text("nextSound"));
    next.setTooltip(language.text("nextSound"));
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
    const auto font = AVDexedTheme::font(15.0f, "Medium");
    const int width = jmax(jmax(59, getWidth()), roundToInt(GlyphArrangement::getStringWidth(font, text)) + 26);
    const int height = 24;
    const auto knob = parent->getLocalArea(&target, target.getLocalBounds());
    int y = knob.getY() - 15 - height;
    if (y < 0) y = knob.getBottom() + 15;
    setBounds(knob.getCentreX() - width / 2, y, width, height);
    setVisible(true);
    toFront(false);
    if (!animation.isRunning()) {
        lastStep = Time::getMillisecondCounterHiRes();
        animation.start();
    }
    repaint();
}

void AVDexedValueTip::release() {
    held = false;
    holdUntil = Time::getMillisecondCounterHiRes() + 1152.0;   // 72 frames of 16 ms
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

bool AVDexedValueTip::step(double now) {
    // In over 80 ms, out over 160 ms (5 and 10 frames of 16 ms), by elapsed time.
    const float elapsed = float(jlimit(1.0, 100.0, now - lastStep));
    lastStep = now;
    const float before = opacity;
    if (held || now < holdUntil) {
        opacity = jmin(1.0f, opacity + elapsed / 80.0f);
    } else {
        opacity -= elapsed / 160.0f;
        if (opacity <= 0.0f) {
            opacity = 0.0f;
            setVisible(false);
            setSize(0, 0);
            slider = nullptr;
            return false;
        }
    }
    // Held at full opacity nothing changes: no repaint (show() repaints new text).
    if (opacity != before) repaint();
    return true;
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

bool AVDexedPresetList::keyPressed(const KeyPress& key) {
    // Up/Down: the previous / next sound, exactly as the arrows next to the
    // name (from the one that plays, wrapping at the ends). The arrows only
    // browse sounds: no focus ring comes on, and Left/Right do nothing here
    // (left to the editor they would light the rings and move nothing).
    if (const int direction = avSoundStepOf(key)) {
        if (onStep) onStep(direction);
        return true;
    }
    if (avIsSidewaysArrow(key)) return true;
    // An arrow with Cmd, Ctrl or Alt goes up to the editor untouched: ListBox
    // would move the highlight without loading, and the rings would come on.
    if (key.isKeyCode(KeyPress::upKey) || key.isKeyCode(KeyPress::downKey)
        || key.isKeyCode(KeyPress::leftKey) || key.isKeyCode(KeyPress::rightKey))
        return false;
    // Page Up/Down, Home/End: keyboard navigation of the list itself, which
    // shows the focus rings (:focus-visible), and loads the row they reach.
    if (AVDexedTheme::noteKeyboardNavigation(key))
        if (auto* top = getTopLevelComponent()) top->repaint();
    const int before = getSelectedRow();
    const bool handled = ListBox::keyPressed(key);
    const bool jump = key.isKeyCode(KeyPress::pageUpKey) || key.isKeyCode(KeyPress::pageDownKey)
        || key.isKeyCode(KeyPress::homeKey) || key.isKeyCode(KeyPress::endKey);
    if (handled && jump && getSelectedRow() != before && getSelectedRow() >= 0 && onKeyboardSelection)
        onKeyboardSelection(getSelectedRow());
    return handled;
}

bool AVDexedSearchField::keyPressed(const KeyPress& key) {
    // Plain Up/Down only: Shift+Up/Down still select the text, as in any field.
    if (const int direction = avSoundStepOf(key);
        direction != 0 && ! key.getModifiers().isShiftDown() && onStep != nullptr) {
        onStep(direction);
        return true;
    }
    return TextEditor::keyPressed(key);
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
    setOpaque(true);   // paint fills it all (the canvas)
    AVDexedLanguageService::getInstance().addChangeListener(this);
    avBrowserPreferenceChanges.addChangeListener(this);
    avLibraryChanges.addChangeListener(this);
    setLookAndFeel(&playLookAndFeel);
    favoriteRowIcon = avDrawableFromSvg(BinaryData::AVDexed_FavoriteIcon_svg, BinaryData::AVDexed_FavoriteIcon_svgSize);
    favoriteRowIconOff = avGreyDrawableFromSvg(BinaryData::AVDexed_FavoriteIcon_svg, BinaryData::AVDexed_FavoriteIcon_svgSize);
    folderRowIcon = avDrawableFromSvg(BinaryData::AVDexed_FolderIcon_svg, BinaryData::AVDexed_FolderIcon_svgSize);
    loadBrowserPreferences();
    applyLanguage();

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
    // Up/Down while typing walk the sounds found, the field keeps the focus.
    searchField.onStep = [this](int direction) { navigatePreset(direction); };
    addAndMakeVisible(searchField);

    for (auto* button : { &folderActionsButton }) {
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
    addAndMakeVisible(importBankButton);

    programList.setModel(this);
    programList.setRowHeight(34);
    programList.setOutlineThickness(0);
    programList.addMouseListener(this, true);
    programList.onKeyboardSelection = [this](int row) { requestEntryLoad(row); };
    programList.onStep = [this](int direction) { navigatePreset(direction); };
    programList.setWantsKeyboardFocus(true);
    programList.setMultipleSelectionEnabled(false);
    programList.setColour(ListBox::backgroundColourId, Colours::transparentBlack);
    programList.setColour(ListBox::outlineColourId, Colours::transparentBlack);
    addAndMakeVisible(programList);
    for (auto* label : { &browserHelp, &emptyLibraryLabel }) {
        label->setFont(AVDexedTheme::font(10.0f, "Regular"));
        label->setColour(Label::textColourId, AVDexedTheme::textSecondary);
        label->setMinimumHorizontalScale(1.0f);
        addAndMakeVisible(*label);
    }
    browserHelp.setFont(AVDexedTheme::font(10.0f, "Regular"));
    browserHelp.setJustificationType(Justification::topLeft);
    emptyLibraryLabel.setJustificationType(Justification::centred);
    emptyLibraryLabel.setInterceptsMouseClicks(false, false);

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
    presetNavigator.onName = [this] { revealActivePreset(); };
    presetNavigator.onStepMenu = [this](int step) { showAVPresetStepMenu(step); };
    presetNavigator.AVStepController = [this](int step) {
        return processor.getAVPresetStepCC(static_cast<DexedAudioProcessor::AVPresetStep>(step));
    };

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
    AVKnobKeys = std::make_unique<AVKnobSoundKeys>([this](int direction) { navigatePreset(direction); });
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
        slider->addKeyListener(AVKnobKeys.get());
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
    // Moves and removals another Play view made while this processor had no
    // window: followed before the first index, so nothing is guessed by content.
    const auto followed = avFollowedLibraryMoves().find(&processor);
    AVFollowedMoves = followed != avFollowedLibraryMoves().end() ? followed->second : 0;
    followLibraryMoves();
    rebuildFolderPicker();
    reindexLibrary();
    updateBank();
    applyLanguage();
    AVLibraryPoll.startTimer(2000);
}

void AVDexedPlayView::changeListenerCallback(ChangeBroadcaster* source) {
    if (source == &avBrowserPreferenceChanges) {
        loadFavorites();
        rebuildFilter();
        refreshLabels();
    } else if (source == &avLibraryChanges) {
        // Another view of this process changed the disk. Its exact moves come
        // first: this processor then names the right file, which is new to
        // this index. Otherwise the one that did it already re-reads, so this
        // only re-reads when the shape differs.
        if (followLibraryMoves()) {
            reindexLibrary();
            updateBank();
        } else {
            checkLibrarySignature();
        }
    } else {
        applyLanguage();
    }
    repaint();
}

void AVDexedPlayView::showLibraryFolder(const String& relativeFolder) {
    const auto root = libraryRoot();
    auto relative = relativeFolder.replaceCharacter('\\', '/').trimCharactersAtStart("/").trimCharactersAtEnd("/");
    // Keep the spelling on disk ("pianos" typed, "Pianos" there).
    if (relative.isNotEmpty() && root.getChildFile(relative).isDirectory()) {
        auto folder = root;
        for (const auto& part : StringArray::fromTokens(relative, "/", {})) {
            auto next = folder.getChildFile(part);
            for (const auto& entry : RangedDirectoryIterator(folder, false, "*", File::findDirectories))
                if (entry.getFile().getFileName() == part) { next = entry.getFile(); break; }
                else if (entry.getFile().getFileName().equalsIgnoreCase(part)) next = entry.getFile();
            folder = next;
        }
        relative = avRelativeFolder(folder, root);
    }
    searchField.clear();
    const auto key = relative.isEmpty() ? String("loose") : "folder:" + relative;
    if (folderKeyExists(key)) {
        selectedFolderKey = key;
        saveSelectedFolder();
    }
    rebuildFolderPicker();
    libraryChanged();
}

File AVDexedPlayView::getSelectedLibraryFolder() const {
    if (! selectedFolderKey.startsWith("folder:") || ! folderKeyExists(selectedFolderKey)) return {};
    return libraryRoot().getChildFile(selectedFolderKey.fromFirstOccurrenceOf("folder:", false, false));
}

void AVDexedPlayView::refreshLibrary() {
    loadFavorites();
    libraryChanged();
}

void AVDexedPlayView::revealWhenIndexed(const File& file) {
    AVRevealFile = file;
}

void AVDexedPlayView::revealActivePreset() {
    searchField.clear();
    rebuildFilter();
    // Stay in the folder when it holds the sound; otherwise show everything.
    const bool shown = std::any_of(visibleEntries.begin(), visibleEntries.end(),
                                   [this](const PresetEntry& entry) { return isActiveEntry(entry); });
    if (! shown && selectedFolderKey != "all") setSelectedFolder("all");
    selectActivePreset();
    programList.scrollToEnsureRowIsOnscreen(programList.getSelectedRow());
    followActiveRow();
}

void AVDexedPlayView::applyLanguage() {
    auto& language = AVDexedLanguageService::getInstance();
    repaint(AVHeaderBounds);   // "Sonidos • N" is painted
    searchField.setTextToShowWhenEmpty(language.text("search"), AVDexedTheme::textSecondary);
    searchField.setTitle(language.text("search"));
    searchField.setTooltip(language.text("browserHelp"));
    programList.setTooltip(language.text("browserHelp"));
    emptyLibraryLabel.setText(language.text("noPresets"), dontSendNotification);
    folderActionsButton.setButtonText(language.text("organize"));
    folderActionsButton.setTooltip(language.text("manageFolders"));
    importBankButton.setButtonText(language.text("importBank"));
    importBankButton.setTitle(language.text("importBank"));
    importBankButton.setTooltip(language.text("importBankHelp"));
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
    effectSettingsWindow.reset();
    AVLibraryPoll.stopTimer();
    // The processor outlives its window: the next one carries on from here.
    avFollowedLibraryMoves()[&processor] = AVFollowedMoves;
    if (libraryIndexCancelled != nullptr) libraryIndexCancelled->store(true);
    libraryIndexPool.removeAllJobs(true, 5000);
    AVDexedLanguageService::getInstance().removeChangeListener(this);
    avBrowserPreferenceChanges.removeChangeListener(this);
    avLibraryChanges.removeChangeListener(this);
    setLookAndFeel(nullptr);
    for (auto* slider : { &volumeSlider, &panSlider, &brightnessSlider, &reverbSlider, &delaySlider, &chorusSlider }) {
        slider->removeMouseListener(AVContextMenu.get());
        slider->removeKeyListener(AVKnobKeys.get());
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
        AVDexedComponentFade::cancel(*overlay);
        overlay->exitModalState(0);
        // GOZO dialog timing: in ~5 frames, out ~7 frames.
        AVDexedComponentFade::fadeOut(*overlay, 117);
        Timer::callAfterDelay(1, [safeThis] {
            if (safeThis && safeThis->effectSettingsWindow && !safeThis->effectSettingsWindow->isVisible())
                safeThis->effectSettingsWindow.reset();
        });
    };
    // A photo of the whole editor, as Settings and the dialogs use: the brand
    // block of the top bar is painted by the editor itself, under the frame, so
    // a photo of the frame alone left it sharp behind the blur.
    Image photo;
    if (auto* editor = findParentComponentOfClass<AudioProcessorEditor>())
        photo = editor->createComponentSnapshot(editor->getLocalBounds()).rescaled(parent->getWidth(), parent->getHeight());
    else
        photo = parent->createComponentSnapshot(parent->getLocalBounds());
    effectSettingsWindow = std::make_unique<AVEffectOverlay>(processor.parameters, static_cast<AVEffectKind>(kind), *parent,
                                                             photo, close);
    // Fade first: enterModalState() makes the overlay visible, and fadeIn() does
    // nothing to a component that is already visible and opaque.
    parent->addChildComponent(*effectSettingsWindow);
    AVDexedComponentFade::fadeIn(*effectSettingsWindow, 83);
    effectSettingsWindow->enterModalState(true);
}

void AVDexedPlayView::visibilityChanged() {
    if (! isVisible()) {
        effectSettingsWindow.reset();
        return;
    }
    // Back from Advanced (the bank manager may have written files): re-read
    // now instead of waiting for the poll. Unchanged files come from the cache.
    loadFavorites();
    if (! libraryIndexing) reindexLibrary();
}

void AVDexedPlayView::paint(Graphics& g) {
    backgroundLayer.draw(g, *this, getLocalBounds().toFloat(), [this](Graphics& bg) {
        bg.fillAll(AVDexedTheme::canvas);
        // A soft ocean light behind the algorithm plays the role GOZO's
        // instrument photo plays: one focal point, no boxes around it.
        if (!heroBounds.isEmpty()) {
            const auto centre = heroBounds.toFloat().getCentre();
            ColourGradient glow(AVDexedTheme::oceanBottom.withAlpha(0.42f), centre.x, centre.y,
                                Colours::transparentBlack, centre.x + float(heroBounds.getWidth()) * 0.62f, centre.y, true);
            bg.setGradientFill(glow);
            bg.fillRect(getLocalBounds());
        }
        AVDexedTheme::panel(bg, browserCard.toFloat(), 22.0f);
    });
    // The rest lies on the card: a glow frame (or the knobs) never reaches it.
    if (!g.clipRegionIntersects(browserCard)) return;
    // Card title as in GOZO (SemiBold 18, grey gradient), the count in the
    // same line and weight: "Sonidos • 1088", centred. "…" until the first
    // index arrives.
    const auto count = libraryIndexing && libraryEntries.empty() ? String::fromUTF8("…")
                                                                  : String(static_cast<int>(visibleEntries.size()));
    AVDexedTheme::drawGreyText(g, AVDexedLanguageService::getInstance().format("soundsCount", {{"count", count}}),
                               AVDexedTheme::font(18.0f, "SemiBold"), AVHeaderBounds.toFloat(), Justification::centred);
    AVDexedTheme::fillBox(g, searchBounds.toFloat(), 16.0f, searchField.hasKeyboardFocus(true) || searchField.isMouseOver(true));
    // Magnifier: ring and handle, drawn rather than taken from a font.
    const auto lens = Rectangle<float>(12.0f, 12.0f).withPosition(float(searchBounds.getX()) + 13.0f,
                                                                   float(searchBounds.getCentreY()) - 7.0f);
    g.setColour(AVDexedTheme::textSecondary);
    g.drawEllipse(lens, 1.6f);
    g.drawLine(lens.getRight() - 1.5f, lens.getBottom() - 1.5f, lens.getRight() + 3.0f, lens.getBottom() + 3.0f, 1.8f);
}

void AVDexedPlayView::resized() {
    backgroundLayer.invalidate();
    if (heroAlgorithm == nullptr || knobCaptions.back() == nullptr) return;
    const int height = getHeight();
    // Browser: one GOZO card on the left, from the top bar to the knob row.
    browserCard = Rectangle<int>(30, 12, 360, jmax(200, height - 24));
    auto card = browserCard.reduced(20, 16);
    AVHeaderBounds = card.removeFromTop(28);   // same 28 px as before: the search field stays put
    card.removeFromTop(10);
    searchBounds = card.removeFromTop(32);
    searchField.setBounds(searchBounds.withTrimmedLeft(30).withTrimmedRight(8));
    card.removeFromTop(10);
    auto folders = card.removeFromTop(30);
    folderActionsButton.setBounds(folders.removeFromRight(92));
    folders.removeFromRight(8);
    folderPickerButton.setBounds(folders);
    card.removeFromTop(10);
    importBankButton.setBounds(card.removeFromBottom(32));
    card.removeFromBottom(10);
    programList.setBounds(card);
    followActiveRow();
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
    const bool active = isActiveEntry(entry);
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
    // Name and source share the row between the number and the star. The
    // name comes first: the source takes what the name leaves, never less
    // than ~40% when both are long, and a long one is cut with "…" at a
    // readable size instead of shrinking into a smudge.
    const float nameX = 42.0f, gap = 10.0f;
    const float span = jmax(0.0f, starX - 6.0f - nameX);
    const auto nameFont = AVDexedTheme::font(15.0f, active ? "SemiBold" : "Medium");
    const auto sourceFont = AVDexedTheme::font(13.0f, "Medium");
    const auto source = AVDisplaySourceName(entry.source);
    // Each width keeps 2 px over the text's own: avFitLine wants 1 px of slack.
    const float nameNeeds = GlyphArrangement::getStringWidth(nameFont, entry.name) + 2.0f;
    const float sourceWidth = source.isEmpty() ? 0.0f
        : jmin(GlyphArrangement::getStringWidth(sourceFont, source) + 2.0f, jmax(span * 0.4f, span - gap - nameNeeds));
    const float nameWidth = jmax(10.0f, span - (sourceWidth > 0.0f ? sourceWidth + gap : 0.0f));
    const auto [fittedNameFont, fittedName] = avFitLine(entry.name, nameFont, nameWidth, 0.85f);
    const auto nameArea = Rectangle<float>(nameX, 0.0f, nameWidth, float(height));
    if (active) AVDexedTheme::drawOceanText(g, fittedName, fittedNameFont, nameArea, Justification::centredLeft);
    else AVDexedTheme::drawGreyText(g, fittedName, fittedNameFont, nameArea, Justification::centredLeft);
    if (sourceWidth > 0.0f) {
        const auto [fittedSourceFont, fittedSource] = avFitLine(source, sourceFont, sourceWidth, 1.0f);
        AVDexedTheme::drawGreyText(g, fittedSource, fittedSourceFont,
                                   { starX - 6.0f - sourceWidth, 0.0f, sourceWidth, float(height) },
                                   Justification::centredRight, 0.85f);
    }
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
    // Programmatic selection (search refresh, revealing the active sound,
    // context menus) only moves the highlight. Loads come from a click, Enter,
    // the arrow keys (AVDexedPresetList) or the previous/next buttons.
}

void AVDexedPlayView::requestEntryLoad(int row) {
    if (!isPositiveAndBelow(row, static_cast<int>(visibleEntries.size()))) return;
    const auto& entry = visibleEntries[static_cast<size_t>(row)];
    // The same bank in another folder is another row: only this exact file and
    // slot counts as "already playing". The library copy that only stands in
    // for a bank without file (a SysEx dump) loads for real: from its file,
    // with the settings that file keeps for this slot.
    const bool standIn = ! entry.isUser && entry.bank != nullptr && AVStandInBankFile != File()
                         && entry.bank->file == AVStandInBankFile
                         && processor.activeFileCartridge.getFullPathName().isEmpty();
    if (isActiveEntry(entry) && ! standIn) return;
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
    const auto entry = std::make_shared<PresetEntry>(visibleEntries[static_cast<size_t>(row)]);
    const auto root = libraryRoot();
    const auto file = fileOf(*entry);
    const bool hasFile = file.existsAsFile();
    // Rename and delete are for the user's own .dexedpreset files; banks are
    // managed as files (copy to a folder, Finder) and packs stay untouched.
    const bool ownPreset = entry->isUser && ! entry->readOnly && ! entry->transient && hasFile;
    auto& language = AVDexedLanguageService::getInstance();
    PopupMenu menu;
    menu.addItem(1, language.text("favorite"), entry->key.isNotEmpty(), favoriteKeys.contains(entry->key),
                 avDrawableFromSvg(BinaryData::AVDexed_FavoriteIcon_svg, BinaryData::AVDexed_FavoriteIcon_svgSize));
    menu.addSeparator();
    menu.addItem(2, language.text("newFolderWithSound"), hasFile);
    PopupMenu copies;
    StringArray targets;
    if (hasFile)
        for (const auto& folder : AVDexedLibrary::listFolders(root, true)) {
            const auto relative = avRelativeFolder(folder, root);
            if ((! entry->transient && avSameFolder(relative, entry->folder)) || AVDexedLibrary::isReadOnly(folder, root)) continue;
            copies.addItem(100 + targets.size(), folderLabel("folder:" + relative), true, false,
                           avDrawableFromSvg(BinaryData::AVDexed_FolderIcon_svg, BinaryData::AVDexed_FolderIcon_svgSize));
            targets.add(folder.getFullPathName());
        }
    menu.addSubMenu(language.text("copyToFolder"), copies, ! targets.isEmpty());
    if (entry->isUser) {
        menu.addSeparator();
        menu.addItem(3, language.text("renameSound"), ownPreset);
        menu.addItem(4, language.text("deleteSound"), ownPreset);
    }
    menu.addSeparator();
    menu.addItem(5, avRevealText(), hasFile);
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    menu.setLookAndFeel(&getLookAndFeel());  // menus never inherit it from their target
    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&programList).withMousePosition(),
                       [safeThis, entry, targets, file](int result) {
        if (safeThis == nullptr || result == 0) return;
        if (result == 1) safeThis->toggleFavoriteKey(entry->key);
        else if (result == 2) safeThis->promptNewFolder(entry);
        else if (result == 3) safeThis->promptRenameSound(*entry);
        else if (result == 4) safeThis->promptDeleteSound(*entry);
        else if (result == 5) file.revealToUser();
        else if (isPositiveAndBelow(result - 100, targets.size())) safeThis->copyToFolder(*entry, File(targets[result - 100]));
    });
}

void AVDexedPlayView::returnKeyPressed(int row) { requestEntryLoad(row); }

void AVDexedPlayView::rebuildFilter() {
    visibleEntries.clear();
    const auto words = StringArray::fromTokens(searchField.getText().trim(), " ", "\"");
    const auto matches = [&words](const PresetEntry& entry) {
        // The bank's file name and the folder are searchable too ("Dexed_01", "Pianos").
        const auto searchable = entry.name + " " + entry.source + " " + entry.folder;
        for (const auto& word : words)
            if (!searchable.containsIgnoreCase(word)) return false;
        return true;
    };
    // A sound playing from outside the library (an old session, a SysEx dump)
    // is listed on top of "Todos los sonidos" only.
    if (selectedFolderKey == "all")
        for (const auto& entry : transientEntries)
            if (matches(entry)) visibleEntries.push_back(entry);
    const auto folder = selectedFolderKey.fromFirstOccurrenceOf("folder:", false, false);
    std::map<String, size_t> favoriteRows;
    for (const auto& entry : libraryEntries) {
        if (!matches(entry)) continue;
        bool accepted = selectedFolderKey == "all";
        if (selectedFolderKey == "favorites") {
            if (!favoriteKeys.contains(entry.key)) continue;
            // Favourites are content: the same sound in two folders is one
            // favourite, listed once (as the copy that plays, if one does).
            const auto shown = favoriteRows.find(entry.key);
            if (shown != favoriteRows.end()) {
                if (isActiveEntry(entry)) visibleEntries[shown->second] = entry;
                continue;
            }
            favoriteRows[entry.key] = visibleEntries.size();
            accepted = true;
        }
        else if (selectedFolderKey == "loose") accepted = entry.folder.isEmpty();
        else if (folder.isNotEmpty()) accepted = avInFolder(entry.folder, folder);
        if (accepted) visibleEntries.push_back(entry);
    }
    programList.updateContent();
    repaint(AVHeaderBounds);   // the count in "Sonidos • N"
    emptyLibraryLabel.setText(AVDexedLanguageService::getInstance().text(libraryIndexing ? "indexingPresets" : "noPresets"), dontSendNotification);
    emptyLibraryLabel.setVisible(visibleEntries.empty());
    selectActivePreset();
}

String AVDexedPlayView::activePresetKey() const {
    // Content identity of what plays: the favourite star in the top bar.
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
    return "bank:" + AVDexedLibrary::bankFingerprint(processor.currentCart) + avProgramSuffix(processor.getCurrentProgram());
}

bool AVDexedPlayView::isActiveEntry(const PresetEntry& entry) const {
    if (!processor.hasAVKnownPresetOrigin()) return false;
    const auto presetPath = processor.getAVPresetPath();
    const auto presetId = processor.getAVPresetId();
    if (presetPath.getFullPathName().isNotEmpty() || presetId.isNotEmpty())
        return entry.isUser && (presetPath.getFullPathName().isNotEmpty() ? entry.file == presetPath
                                                                          : entry.key == "user:" + presetId);
    if (entry.isUser || entry.bank == nullptr || entry.program != processor.getCurrentProgram()) return false;
    const auto& activeFile = processor.activeFileCartridge;
    if (activeFile.getFullPathName().isNotEmpty()) return entry.bank->file == activeFile;
    // A bank without file: its transient row, or the library copy standing in for it.
    return entry.transient || (AVStandInBankFile != File() && entry.bank->file == AVStandInBankFile);
}

String AVDexedPlayView::activeIdentity() const {
    if (!processor.hasAVKnownPresetOrigin()) return {};
    const auto presetPath = processor.getAVPresetPath();
    if (presetPath.getFullPathName().isNotEmpty() || processor.getAVPresetId().isNotEmpty())
        return "preset|" + presetPath.getFullPathName() + "|" + processor.getAVPresetId();
    return "bank|" + processor.activeFileCartridge.getFullPathName() + "|" + String(processor.getCurrentProgram());
}

void AVDexedPlayView::selectActivePreset() {
    programList.deselectAllRows();
    for (int row = 0; row < static_cast<int>(visibleEntries.size()); ++row)
        if (isActiveEntry(visibleEntries[static_cast<size_t>(row)])) {
            // No scrolling here: a re-read of the library must not move the list.
            programList.selectRow(row, true);
            return;
        }
}

void AVDexedPlayView::followActiveRow() {
    // Runs from resized(), updateBank() and each publish of the index. The
    // sound counts as shown only once its row is there and selected: when the
    // window opens, the list is sized before the first index has any rows, and
    // a sound in a folder not shown, or still waiting for the index, is
    // followed when its row appears. Not before the list has its height:
    // scrolling a list with no height leaves it one row down, hiding the very
    // row it meant to show.
    const auto identity = activeIdentity();
    if (identity == AVShownActiveIdentity || programList.getHeight() <= programList.getRowHeight()) return;
    const int row = programList.getSelectedRow();
    if (! isPositiveAndBelow(row, static_cast<int>(visibleEntries.size()))
        || ! isActiveEntry(visibleEntries[static_cast<size_t>(row)]))
        return;
    AVShownActiveIdentity = identity;
    programList.scrollToEnsureRowIsOnscreen(row);
}

void AVDexedPlayView::navigatePreset(int direction) {
    if (visibleEntries.empty()) return;
    const int count = static_cast<int>(visibleEntries.size());
    int row = -1;
    const int selected = programList.getSelectedRow();
    if (isPositiveAndBelow(selected, count) && isActiveEntry(visibleEntries[static_cast<size_t>(selected)]))
        row = selected;
    else
        for (int i = 0; i < count; ++i)
            if (isActiveEntry(visibleEntries[static_cast<size_t>(i)])) { row = i; break; }
    if (row < 0) row = direction > 0 ? -1 : 0;
    row = (row + direction + count) % count;
    programList.selectRow(row, true);
    programList.scrollToEnsureRowIsOnscreen(row);
    requestEntryLoad(row);
}

bool AVDexedPlayView::keyPressed(const KeyPress& key) {
    // Keys no child of the view took: the folder picker, "Organizar",
    // "Importar banco" and the effect captions are buttons, which ignore the
    // arrows. Up/Down step through the sounds from there too, and Left/Right
    // do nothing, so no arrow reaches the editor to light the focus rings.
    if (const int direction = avSoundStepOf(key)) {
        navigatePreset(direction);
        return true;
    }
    return avIsSidewaysArrow(key);
}

void AVDexedPlayView::showAVPresetStepMenu(int step) {
    if (step != 0 && step != 1) return;
    auto& language = AVDexedLanguageService::getInstance();
    const auto which = static_cast<DexedAudioProcessor::AVPresetStep>(step);
    const int cc = processor.getAVPresetStepCC(which);
    PopupMenu menu;
    menu.addItem(1, language.text("presetStepLearn"));
    if (cc >= 0) menu.addItem(2, language.format("presetStepClear", {{"cc", String(cc)}}));
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    menu.setLookAndFeel(&getLookAndFeel());  // menus never inherit it from their target
    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&presetNavigator).withMousePosition(),
                       [safeThis, step, which](int result) {
        if (safeThis == nullptr || result == 0) return;
        if (result == 2) {
            safeThis->processor.clearAVPresetStepCC(which);
            return;
        }
        // The editor shows the waiting dialog and starts the learn. Without
        // it the learn still starts, and the footer says what to do.
        if (safeThis->onPresetStepLearnRequested) {
            safeThis->onPresetStepLearnRequested(step);
        } else {
            safeThis->processor.startAVPresetStepLearn(which);
            safeThis->notify(AVDexedLanguageService::getInstance().text("learnWaiting"));
        }
    });
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
    // The bank as its file holds it now, not the index's copy: that copy can
    // predate a save made since the row was clicked ("Guardar" in the "save
    // changes?" prompt writes this very bank first), and installing it would
    // undo that save in memory (old voice, settings that no longer match, and
    // the next "Guardar" sent to "Guardar como…"). A transient row is the
    // processor's own bank, read from it: it may hold a sound stored only in
    // the session, which its file does not have.
    const auto bank = entry.transient ? entry.bank->cartridge
                                      : AVDexedLibrary::currentBankOf(entry.bank->file, entry.bank->cartridge);
    // The bank and its file first, then the program: setCurrentProgram applies
    // the sound's embedded settings and takes the "unmodified" baseline.
    processor.setActiveBank(bank, entry.bank->file);
    processor.setCurrentProgram(entry.program);
    processor.captureAVPresetBaseline();
    processor.updateHostDisplay();
    updateBank();
}

void AVDexedPlayView::updateBank() {
    const auto transientBefore = AVTransientIdentity;
    syncTransientEntries();
    if (AVTransientIdentity != transientBefore) rebuildFilter();
    else selectActivePreset();
    followActiveRow();
    programList.repaint();
    refreshLabels();
}

void AVDexedPlayView::syncTransientEntries() {
    // Moves and removals another Play view of this process made come first:
    // the processor then names the exact file (new to this index, so it is
    // read again) and nothing below has to guess it by content.
    if (followLibraryMoves()) reindexLibrary();
    AVStandInBankFile = File();
    const auto root = libraryRoot();
    std::vector<PresetEntry> rows;
    String identity;
    const auto insideLibrary = [&root](const File& file) {
        if (! file.isAChildOf(root)) return false;
        for (auto folder = file.getParentDirectory(); folder.isAChildOf(root); folder = folder.getParentDirectory())
            if (AVDexedLibrary::isIgnoredName(folder.getFileName())) return false;
        return true;
    };
    if (processor.hasAVKnownPresetOrigin()) {
        const auto presetPath = processor.getAVPresetPath();
        if (presetPath.getFullPathName().isNotEmpty()) {
            bool indexed = std::any_of(libraryEntries.begin(), libraryEntries.end(),
                                       [&presetPath](const PresetEntry& entry) { return entry.isUser && entry.file == presetPath; });
            // A preset just saved into the library is waiting for the index, not outside it.
            const bool pending = libraryIndexing && presetPath.existsAsFile() && insideLibrary(presetPath);
            const auto presetId = processor.getAVPresetId();
            if (! indexed && ! pending && ! libraryIndexing && presetId.isNotEmpty()
                && ! presetPath.existsAsFile() && ! isRemovedFile(presetPath)) {
                // Moved or renamed where this view did not see it (Finder,
                // another AV Dexed or DAW): the preset keeps its id inside the
                // file, so it is found by it, as a bank is found by its
                // content. "Copiar a carpeta" makes copies with the same id, so
                // only a clear match is followed (the one copy, or the one that
                // kept its file name and folders); else it stays a transient row.
                Array<File> copies;
                StringArray names;
                for (const auto& entry : libraryEntries)
                    if (entry.isUser && entry.key == "user:" + presetId && ! copies.contains(entry.file)) {
                        copies.add(entry.file);
                        names.add(entry.name);
                    }
                const auto match = avLikelyCopy(presetPath, copies, root);
                if (match != File()) {
                    // The name the file holds now: renamed in another AV Dexed, it
                    // was written inside it too, and "Guardar" must not undo that.
                    const auto storedName = names[copies.indexOf(match)];
                    const bool wasModified = processor.isAVPresetModified();
                    processor.setAVPresetContext(storedName.isNotEmpty() ? storedName : processor.getAVPresetName(),
                                                 presetId, match);
                    if (! wasModified) processor.captureAVPresetBaseline();
                    indexed = true;
                }
            }
            if (! indexed && ! pending) {
                identity = "preset|" + presetPath.getFullPathName() + "|" + processor.getAVPresetName();
                PresetEntry entry;
                entry.file = presetPath;
                entry.name = processor.getAVPresetName();
                // A removed preset's hidden copy has no folder worth naming.
                entry.source = isRemovedFile(presetPath) ? String() : presetPath.getParentDirectory().getFileName();
                entry.key = activePresetKey();
                entry.isUser = true;
                entry.transient = true;
                rows.push_back(std::move(entry));
            }
        } else if (processor.getAVPresetId().isEmpty() && processor.currentCart.containsData()) {
            const auto activeFile = processor.activeFileCartridge;
            const auto fingerprint = AVDexedLibrary::bankFingerprint(processor.currentCart);
            const auto indexedAt = [this](const File& file) {
                return std::any_of(libraryEntries.begin(), libraryEntries.end(), [&file](const PresetEntry& entry) {
                    return ! entry.isUser && entry.bank != nullptr && entry.bank->file == file;
                });
            };
            bool indexed = activeFile.getFullPathName().isNotEmpty() && indexedAt(activeFile);
            const bool pending = libraryIndexing && activeFile.existsAsFile() && insideLibrary(activeFile);
            const bool withoutFile = activeFile.getFullPathName().isEmpty();
            // The library's copies of this very bank, in index order, each file once.
            const auto identicalBanks = [this, &fingerprint] {
                Array<File> files;
                for (const auto& entry : libraryEntries)
                    if (! entry.isUser && entry.bank != nullptr && entry.bank->fingerprint == fingerprint)
                        files.addIfNotAlreadyThere(entry.bank->file);
                return files;
            };
            if (withoutFile) {
                // Never named (a SysEx dump, an old session, the built-in copy
                // of Dexed_01) but the library holds the very same bank: its
                // first copy stands for it (highlighted row, its name in the
                // centre, the favourite star). The processor keeps no file:
                // bound to that file it would keep this sound's settings (the
                // defaults, a bank without file has none) and "Guardar" would
                // write them over the ones the file keeps for this slot.
                // Without it, "Guardar" goes to "Guardar como…".
                const auto copies = identicalBanks();
                if (! copies.isEmpty()) {
                    AVStandInBankFile = copies.getFirst();
                    indexed = true;
                    // No rows, but a new identity: the list is rebuilt when the
                    // copy standing in changes (Favoritos lists the copy that plays).
                    identity = "stand-in|" + AVStandInBankFile.getFullPathName();
                }
            } else if (! indexed && ! pending && ! libraryIndexing && ! isRemovedFile(activeFile)
                       && (! activeFile.existsAsFile() || ! insideLibrary(activeFile))) {
                // Its file moved (a folder renamed in Finder or in another DAW,
                // a 0.1.0 session) or lives outside the library, and the library
                // holds the very same bank: play it from there (same voices, so
                // nothing changes), but only when it is clear which copy it is
                // (the one copy, or the one that kept its file name and
                // folders). With two equally likely copies "Guardar" would write
                // into one the user never chose: it stays a transient row and
                // goes to "Guardar como…". Not after the user removed it here
                // either (it then points into .deleted).
                const auto match = avLikelyCopy(activeFile, identicalBanks(), root);
                if (match != File()) {
                    const Cartridge bank = processor.currentCart;   // a copy: setActiveBank reloads currentCart
                    processor.setActiveBank(bank, match);
                    indexed = true;
                }
            }
            if (! indexed && ! pending) {
                identity = "bank|" + activeFile.getFullPathName() + "|" + fingerprint;
                if (identity == AVTransientIdentity && ! transientEntries.empty()) return;
                auto bank = std::make_shared<AVPresetBank>();
                bank->cartridge = processor.currentCart;
                bank->file = activeFile;
                bank->fingerprint = fingerprint;
                StringArray names;
                bank->cartridge.getProgramNames(names);
                for (int i = 0; i < names.size(); ++i) {
                    PresetEntry entry;
                    entry.program = i;
                    entry.name = names[i];
                    entry.source = withoutFile ? AVDexedLanguageService::getInstance().text("unsavedBank")
                                               : activeFile.getFileNameWithoutExtension();
                    entry.key = "bank:" + fingerprint + avProgramSuffix(i);
                    entry.bank = bank;
                    entry.transient = true;
                    rows.push_back(std::move(entry));
                }
            }
        }
    }
    if (identity == AVTransientIdentity && rows.size() == transientEntries.size()) return;
    AVTransientIdentity = identity;
    transientEntries = std::move(rows);
}

void AVDexedPlayView::reindexLibrary() {
    libraryIndexing = true;
    rebuildFilter();
    if (libraryIndexCancelled != nullptr) libraryIndexCancelled->store(true);
    libraryIndexCancelled = std::make_shared<std::atomic<bool>>(false);
    const auto cancelled = libraryIndexCancelled;
    const auto generation = ++libraryGeneration;
    const auto root = libraryRoot();
    // In-memory cache: a file with the same path, size and time is not read again.
    std::map<String, PresetEntry> cachedPresets;
    std::map<String, std::shared_ptr<AVPresetBank>> cachedBanks;
    for (const auto& entry : libraryEntries) {
        if (entry.isUser) cachedPresets[entry.file.getFullPathName()] = entry;
        else if (entry.bank != nullptr) cachedBanks[entry.bank->file.getFullPathName()] = entry.bank;
    }
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    libraryIndexPool.addJob([cancelled, generation, root, cachedPresets = std::move(cachedPresets),
                             cachedBanks = std::move(cachedBanks), safeThis] {
        // Taken before the walk: anything that changes during it shows at the next poll.
        auto signature = AVDexedLibrary::librarySignature(root);
        std::vector<PresetEntry> entries;
        StringPairArray carried;
        std::map<String, bool> readOnlyFolders;
        const auto carryFavorites = [&carried](const AVPresetBank& before, const AVPresetBank& after) {
            // The same file with new content (a sound saved into its slot, an
            // edit in the bank manager): favourites follow the unchanged voices,
            // and the one voice that changed when only one did.
            if (before.fingerprint == after.fingerprint) return;
            int changed = 0;
            for (int slot = 0; slot < 32; ++slot)
                if (AVDexedLibrary::voiceHash(before.cartridge, slot) != AVDexedLibrary::voiceHash(after.cartridge, slot)) ++changed;
            for (int slot = 0; slot < 32; ++slot)
                if (changed == 1 || AVDexedLibrary::voiceHash(before.cartridge, slot) == AVDexedLibrary::voiceHash(after.cartridge, slot))
                    carried.set("bank:" + before.fingerprint + avProgramSuffix(slot), "bank:" + after.fingerprint + avProgramSuffix(slot));
        };
        for (const auto& file : AVDexedLibrary::findSoundFiles(root)) {
            if (cancelled->load()) return;
            const auto directory = file.getParentDirectory().getFullPathName();
            if (readOnlyFolders.count(directory) == 0) readOnlyFolders[directory] = AVDexedLibrary::isReadOnly(file, root);
            const bool readOnly = readOnlyFolders[directory];
            const auto folder = AVDexedLibrary::folderOf(file, root);
            const auto path = file.getFullPathName();
            const auto fileSize = file.getSize();
            const auto modified = file.getLastModificationTime().toMilliseconds();
            if (file.getFileExtension().equalsIgnoreCase(".syx")) {
                std::shared_ptr<AVPresetBank> bank;
                const auto cached = cachedBanks.find(path);
                if (cached != cachedBanks.end() && cached->second->fileSize == fileSize
                    && cached->second->modificationTime == modified) {
                    bank = cached->second;
                } else {
                    Cartridge cartridge;
                    if (!AVDexedLibrary::readBank(file, cartridge)) continue;   // not a DX7 bank: not a sound
                    bank = std::make_shared<AVPresetBank>();
                    bank->cartridge = cartridge;
                    bank->file = file;
                    bank->fileSize = fileSize;
                    bank->modificationTime = modified;
                    bank->fingerprint = AVDexedLibrary::bankFingerprint(cartridge);
                    // Raw bytes as read (the const accessor does not rewrite the header), as in 0.1.0.
                    bank->legacyFingerprint = AVDexedLibrary::stableHash(static_cast<const Cartridge&>(cartridge).getVoiceSysex(), SYSEX_SIZE);
                    bank->legacyIdentityPrefixes.add("bank:" + bank->legacyFingerprint + ":");
                    if (cached != cachedBanks.end()) carryFavorites(*cached->second, *bank);
                }
                StringArray names;
                Cartridge(bank->cartridge).getProgramNames(names);
                for (int i = 0; i < names.size(); ++i) {
                    PresetEntry entry;
                    entry.program = i;
                    entry.name = names[i];
                    entry.source = file.getFileNameWithoutExtension();
                    entry.key = "bank:" + bank->fingerprint + avProgramSuffix(i);
                    entry.isStock = readOnly;
                    entry.readOnly = readOnly;
                    entry.bank = bank;
                    entry.folder = folder;
                    entries.push_back(std::move(entry));
                }
                continue;
            }
            const auto cached = cachedPresets.find(path);
            if (cached != cachedPresets.end() && cached->second.indexedFileSize == fileSize
                && cached->second.indexedModificationTime == modified && cached->second.key.isNotEmpty()) {
                auto entry = cached->second;
                entry.folder = folder;
                entry.readOnly = entry.isStock = readOnly;
                entries.push_back(std::move(entry));
                continue;
            }
            MemoryBlock contents;
            if (fileSize <= 0 || fileSize > 32 * 1024 * 1024 || !file.loadFileAsData(contents)) continue;
            auto xml = AudioProcessor::getXmlFromBinary(contents.getData(), static_cast<int>(contents.getSize()));
            if (xml == nullptr) continue;
            const auto tree = ValueTree::fromXml(*xml);
            const auto stableId = tree.getProperty("libraryPresetId").toString().trim();
            auto name = tree.getChildWithName("session").getProperty("avPresetName").toString();
            if (name.isEmpty()) name = file.getFileNameWithoutExtension();
            PresetEntry entry;
            entry.file = file;
            entry.name = name;
            // Its folder; a loose preset has none (not the data folder's name).
            entry.source = folder.isEmpty() ? String() : file.getParentDirectory().getFileName();
            entry.key = stableId.isNotEmpty() ? "user:" + stableId
                                              : "user-content:" + avStableStateHash(contents.getData(), contents.getSize());
            entry.isUser = true;
            entry.isStock = entry.readOnly = readOnly;
            entry.indexedFileSize = fileSize;
            entry.indexedModificationTime = modified;
            entry.folder = folder;
            entries.push_back(std::move(entry));
        }
        if (cancelled->load()) return;
        MessageManager::callAsync([safeThis, generation, entries = std::move(entries), signature, carried]() mutable {
            if (safeThis != nullptr) safeThis->publishLibrary(std::move(entries), generation, signature, carried);
        });
    });
}

void AVDexedPlayView::publishLibrary(std::vector<PresetEntry> entries, unsigned generation, String signature,
                                     StringPairArray carriedFavorites) {
    if (generation != libraryGeneration) return;
    libraryIndexing = false;
    libraryEntries = std::move(entries);
    AVPublishedSignature = signature;
    // Favourites follow their sounds: keys written by 0.1.0 (paths, whole-file
    // hashes, "stock:NN") move to today's keys, and banks rewritten in place
    // pass them on (the old key stays: a copy of the old bank may exist).
    StringArray add, remove;
    const auto follow = [this, &add, &remove](const String& oldKey, const String& newKey, bool keepOld) {
        if (oldKey.isEmpty() || newKey.isEmpty() || oldKey == newKey || ! favoriteKeys.contains(oldKey)) return;
        if (! favoriteKeys.contains(newKey)) add.addIfNotAlreadyThere(newKey);
        if (! keepOld) remove.addIfNotAlreadyThere(oldKey);
    };
    if (! favoriteKeys.isEmpty()) {
        for (const auto& entry : libraryEntries)
            for (const auto& alias : legacyKeysOf(entry))
                follow(alias, entry.key, false);
        const auto& oldKeys = carriedFavorites.getAllKeys();
        const auto& newKeys = carriedFavorites.getAllValues();
        for (int i = 0; i < oldKeys.size(); ++i)
            follow(oldKeys[i], newKeys[i], true);
    }
    if (! add.isEmpty() || ! remove.isEmpty()) updateFavorites(add, remove);
    migrateLegacyFolders();
    syncTransientEntries();
    rebuildFolderPicker();
    rebuildFilter();
    refreshLabels();
    // A sound just saved into another folder than the one shown (the Capas
    // page saves into "User Presets"): its folder opens, so it is not missed.
    if (AVRevealFile != File()) {
        const auto saved = std::find_if(libraryEntries.begin(), libraryEntries.end(),
                                        [this](const PresetEntry& entry) { return entry.isUser && entry.file == AVRevealFile; });
        if (saved != libraryEntries.end()) {
            AVRevealFile = File();
            const auto folder = saved->folder;
            const bool shown = std::any_of(visibleEntries.begin(), visibleEntries.end(),
                                           [this](const PresetEntry& entry) { return isActiveEntry(entry); });
            if (! shown) {
                searchField.clear();
                setSelectedFolder(folder.isNotEmpty() ? "folder:" + folder : String("loose"));
                if (! std::any_of(visibleEntries.begin(), visibleEntries.end(),
                                  [this](const PresetEntry& entry) { return isActiveEntry(entry); }))
                    setSelectedFolder("all");
            }
            programList.scrollToEnsureRowIsOnscreen(jmax(0, programList.getSelectedRow()));
        }
    }
    // The first index after the window opened brings the row that plays.
    followActiveRow();
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
    const PresetEntry* entry = nullptr;
    for (const auto* list : { &transientEntries, &libraryEntries }) {
        for (const auto& candidate : *list)
            if (isActiveEntry(candidate)) { entry = &candidate; break; }
        if (entry != nullptr) break;
    }
    if (nativePreset) {
        collection = entry == nullptr || entry->transient ? activePreset.getParentDirectory().getFileName()
                   : entry->folder.isNotEmpty() ? entry->folder.fromLastOccurrenceOf("/", false, false)
                   : language.text("looseSounds");
        detail = activePreset.getFileNameWithoutExtension();
    } else {
        collection = entry != nullptr && entry->source.isNotEmpty() ? entry->source
                   : processor.hasExternalCartridgeContext() ? processor.activeFileCartridge.getFileNameWithoutExtension()
                   : language.text("unsavedBank");
        detail = language.format("programOfBank", {{"number", String(processor.getCurrentProgram() + 1)}});
    }
    // A file removed from the library (hidden in .deleted, or in the trash) is
    // unavailable too, even while its hidden copy still exists.
    const auto unavailable = [](const File& file) { return ! file.existsAsFile() || isRemovedFile(file); };
    if (nativePreset && isRemovedFile(activePreset)) {
        collection = language.text("sourceUnavailable");   // the top bar keeps the sound's name
        detail = {};
    } else if ((nativePreset && unavailable(activePreset))
               || (!nativePreset && processor.hasExternalCartridgeContext() && unavailable(processor.activeFileCartridge))) {
        detail = language.text("sourceUnavailable") + String::fromUTF8(" · ") + detail;
    }
    if (!processor.hasAVKnownPresetOrigin()) {
        collection = language.text("sourceUnknown");
        detail = {};
    }
    // An expansion reads by its own name here too; the help keeps the full one.
    activeName.setText(AVDisplaySourceName(collection), dontSendNotification);
    activeName.setTooltip(collection);
    sourceName.setText(detail, dontSendNotification);
    sourceName.setTooltip(nativePreset ? activePreset.getFullPathName() : processor.activeFileCartridge.getFullPathName());
    modifiedCurrent = processor.isAVPresetModified();
    modifiedLabel.setText({}, dontSendNotification);
    // With layers, the top bar says which layer Play and Advanced are editing.
    const auto shownName = processor.isAVLayerModeOn()
        ? language.format("layerTitle", {{"number", String(processor.getAVSelectedLayer() + 1)}}) + String::fromUTF8(" · ") + soundName
        : soundName;
    presetNavigator.setPreset(shownName, {}, modifiedCurrent,
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
        AVDexedDim::setAlpha(*knobCaptions[i + 3], bypassed ? 0.55f : 1.0f);
        AVDexedDim::setAlpha(*amounts[i], bypassed ? 0.4f : 1.0f);
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
    // Loading a listed preset changes nothing on disk; a preset that was just
    // saved into the library is new to the index.
    const bool indexed = file.getFullPathName().isEmpty()
        || std::any_of(libraryEntries.begin(), libraryEntries.end(),
                       [&file](const PresetEntry& entry) { return entry.isUser && entry.file == file; });
    if (! indexed && file.isAChildOf(libraryRoot())) libraryChanged();
    updateBank();
}

void AVDexedPlayView::captureCurrentAsBaseline() {
    processor.captureAVPresetBaseline();
    refreshLabels();
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
    if (oldKey.isEmpty() || newKey.isEmpty() || oldKey == newKey) return;
    loadFavorites();
    if (favoriteKeys.contains(oldKey)) updateFavorites(StringArray(newKey), StringArray(oldKey));
}

void AVDexedPlayView::carryAVBankMetadata(const Cartridge& previousBank, const Cartridge& storedBank) {
    if (!previousBank.containsData() || !storedBank.containsData()) return;
    const auto oldFingerprint = AVDexedLibrary::bankFingerprint(previousBank);
    const auto newFingerprint = AVDexedLibrary::bankFingerprint(storedBank);
    if (oldFingerprint == newFingerprint) return;
    // Slot by slot (a sound saved into its own slot changes the whole bank's
    // fingerprint). The old keys stay: a copy of the previous bank may exist.
    loadFavorites();
    StringArray add;
    for (int slot = 0; slot < 32; ++slot) {
        const auto previousKey = "bank:" + oldFingerprint + avProgramSuffix(slot);
        const auto storedKey = "bank:" + newFingerprint + avProgramSuffix(slot);
        if (favoriteKeys.contains(previousKey) && !favoriteKeys.contains(storedKey)) add.add(storedKey);
    }
    if (! add.isEmpty()) {
        updateFavorites(add, {});
        rebuildFilter();
        refreshLabels();
    }
}

//==============================================================================
// Real folders

File AVDexedPlayView::libraryRoot() {
    return DexedAudioProcessor::dexedCartDir;   // == AVDexedLibrary::rootFor(dexedAppDir): the data folder itself
}

File AVDexedPlayView::fileOf(const PresetEntry& entry) {
    if (entry.isUser) return entry.file;
    return entry.bank != nullptr ? entry.bank->file : File();
}

bool AVDexedPlayView::hasLooseSounds() const {
    for (const auto& entry : RangedDirectoryIterator(libraryRoot(), false, "*", File::findFiles))
        if (AVDexedLibrary::isSoundFile(entry.getFile())) return true;
    return false;
}

bool AVDexedPlayView::folderKeyExists(const String& folderKey) const {
    if (folderKey == "all" || folderKey == "favorites") return true;
    if (folderKey == "loose") return hasLooseSounds();
    if (! folderKey.startsWith("folder:")) return false;
    const auto relative = folderKey.fromFirstOccurrenceOf("folder:", false, false);
    const auto parts = StringArray::fromTokens(relative, "/", {});
    if (relative.isEmpty() || parts.isEmpty()) return false;
    for (const auto& part : parts)
        if (AVDexedLibrary::isIgnoredName(part) || part == "..") return false;
    // Same rule as the picker: any visible first-level folder, whatever its
    // name. One made in Finder may be called "Aux" or "***" (sanitizeFolderName
    // refuses those, but it is for names AV Dexed gives); only the root's
    // reserved folders ("Cartridges", "Profiles") are not library folders.
    const auto root = libraryRoot();
    if (! AVDexedLibrary::listFolders(root).contains(root.getChildFile(parts[0]))) return false;
    return root.getChildFile(relative).isDirectory();
}

String AVDexedPlayView::AVDisplaySourceName(const String& name) {
    // AV expansions are named "AV_DEXED_EXPANSION_<real name>": beside each of
    // their sounds only the real name says something.
    // The three tokens are exactly AV, DEXED and EXPANSION (any case), so
    // "AV_DEXED_EXPANSIONS_Pianos" is not one and keeps its whole name.
    const String prefix("AV_DEXED_EXPANSION_");
    if (! name.startsWithIgnoreCase(prefix)) return name;
    const auto rest = name.substring(prefix.length()).trimCharactersAtStart("_").trim();
    return rest.isNotEmpty() ? rest : name;   // the prefix alone: nothing else to show
}

String AVDexedPlayView::folderLabel(const String& folderKey) const {
    auto& language = AVDexedLanguageService::getInstance();
    if (folderKey == "favorites") return language.text("favorites");
    if (folderKey == "loose") return language.text("looseSounds");
    if (folderKey.startsWith("folder:"))
        return folderKey.fromFirstOccurrenceOf("folder:", false, false).replace("/", " / ");
    return language.text("allSounds");
}

void AVDexedPlayView::loadBrowserPreferences() {
    avEditPreferences([this](PropertiesFile& preferences) {
        favoriteKeys = StringArray::fromLines(preferences.getValue("favoritePresetIds"));
        favoriteKeys.removeEmptyStrings();
        auto selected = preferences.getValue("presetBrowserSelectedFolder", "all");
        if (selected == "stock" || selected == "user") {
            selected = "all";   // 0.1.0's virtual collections; the real folders replace them
        } else if (selected != "all" && selected != "favorites" && selected != "loose" && ! selected.startsWith("folder:")) {
            AVLegacySelectedFolder = selected;   // a 0.1.0 virtual folder: it becomes real after the first index
            selected = "all";
        }
        selectedFolderKey = selected;
        // The library is read from disk now; 0.1.0's index cache is obsolete.
        preferences.removeValue("presetBrowserLibraryIndex");
        preferences.removeValue("presetBrowserLibraryIndexVersion");
    });
    AVPreferencesTime = DexedAudioProcessor::getPreferencesFile().getLastModificationTime();
}

void AVDexedPlayView::saveSelectedFolder() {
    AVLegacySelectedFolder = {};   // a folder chosen now replaces the one 0.1.0 had selected
    const auto key = selectedFolderKey;
    avEditPreferences([&key](PropertiesFile& preferences) { preferences.setValue("presetBrowserSelectedFolder", key); });
    AVPreferencesTime = DexedAudioProcessor::getPreferencesFile().getLastModificationTime();
}

StringArray AVDexedPlayView::legacyKeysOf(const PresetEntry& entry) {
    StringArray keys;
    if (entry.isUser) {
        keys.add("user:" + entry.file.getFullPathName().toLowerCase());
    } else if (entry.bank != nullptr) {
        const auto suffix = avProgramSuffix(entry.program);
        keys.add("bank:" + entry.bank->file.getFullPathName().toLowerCase() + suffix);
        for (const auto& alias : entry.bank->legacyIdentityPrefixes)
            keys.add(alias + suffix);
        if (entry.bank->file.getFileNameWithoutExtension().equalsIgnoreCase("Dexed_01"))
            keys.add("stock" + suffix);
    }
    return keys;
}

void AVDexedPlayView::migrateLegacyFolders() {
    // 0.1.0 kept folders as lists of sounds in the preferences. Each one with
    // sounds becomes a real folder with copies of their files (a bank voice
    // brings its whole bank); an empty one is dropped. A sound that is not in
    // the library yet (a bank 0.1.0 played from another drive, copied in later
    // by adoptExternalSounds) stays in the catalog for a later pass, so the
    // catalog shrinks to what is still to come and goes away when it is empty.
    if (AVLegacyFoldersChecked) return;
    const auto root = libraryRoot();
    // Today's key and every older key of each sound (the first file wins).
    std::map<String, const PresetEntry*> byKey;
    for (const auto& entry : libraryEntries) {
        byKey.emplace(entry.key, &entry);
        for (const auto& alias : legacyKeysOf(entry)) byKey.emplace(alias, &entry);
    }
    String selected;
    bool changed = false, finished = true;
    avEditPreferences([this, &root, &byKey, &selected, &changed, &finished](PropertiesFile& preferences) {
        if (! preferences.containsKey("presetBrowserFolders")) return;
        // A sound that is nowhere is only given up when nothing can still bring
        // it: no adoption pending and a library that could be read at all.
        const bool adoptionPending = preferences.getValue("presetBrowserKnownBanks").trim().isNotEmpty()
                                  || preferences.getValue("presetBrowserKnownPresets").trim().isNotEmpty();
        const bool keepMissing = adoptionPending || libraryEntries.empty();
        using Status = AVDexedLibrary::ImportResult::Status;
        std::vector<AVDexedLibrary::LegacyFolder> remaining;
        for (const auto& legacy : AVDexedLibrary::parseLegacyFolders(preferences.getValue("presetBrowserFolders"))) {
            AVDexedLibrary::LegacyFolder waiting { legacy.id, legacy.name, {} };
            std::vector<std::pair<String, const PresetEntry*>> members;
            for (const auto& key : legacy.keys) {
                const auto found = byKey.find(key);
                if (found != byKey.end()) members.emplace_back(key, found->second);
                else if (keepMissing) waiting.keys.add(key);
            }
            if (! members.empty()) {
                auto folder = AVDexedLibrary::writableFolderFor(root, legacy.name);
                if (folder == File()) folder = AVDexedLibrary::writableFolderFor(root, "Folder");
                const bool existed = folder.isDirectory();
                if (folder == File() || ! folder.createDirectory().wasOk()) {
                    remaining.push_back(legacy);
                    continue;
                }
                changed |= ! existed;
                if (legacy.id == AVLegacySelectedFolder) selected = "folder:" + folder.getFileName();
                for (const auto& [key, entry] : members) {
                    const auto source = fileOf(*entry);
                    const auto result = entry->isUser ? AVDexedLibrary::importPreset(source, folder)
                                                      : AVDexedLibrary::importBank(source, folder);
                    if (result.status == Status::copied) changed = true;
                    // Retried at the next pass (imports never duplicate).
                    else if (result.status == Status::failed) waiting.keys.add(key);
                }
            }
            if (! waiting.keys.isEmpty()) remaining.push_back(std::move(waiting));
        }
        if (remaining.empty()) {
            preferences.removeValue("presetBrowserFolders");
        } else {
            preferences.setValue("presetBrowserFolders", AVDexedLibrary::composeLegacyFolders(remaining));
            finished = false;
        }
    });
    // Until the catalog is gone, every new index looks again (a pass that
    // copies nothing changes nothing, so this never loops).
    AVLegacyFoldersChecked = finished;
    if (selected.isNotEmpty() && AVLegacySelectedFolder.isNotEmpty() && folderKeyExists(selected)) {
        selectedFolderKey = selected;
        saveSelectedFolder();
        rebuildFolderPicker();
    }
    if (finished) AVLegacySelectedFolder = {};
    if (changed) libraryChanged();
}

void AVDexedPlayView::rebuildFolderPicker() {
    // A folder renamed or removed in Finder: back to every sound.
    if (! folderKeyExists(selectedFolderKey))
        selectedFolderKey = "all";
    folderPickerButton.setSelection(folderLabel(selectedFolderKey), selectedFolderKey == "favorites");
}

void AVDexedPlayView::setSelectedFolder(const String& folderKey) {
    if (! folderKeyExists(folderKey)) return;
    const bool changed = folderKey != selectedFolderKey;
    selectedFolderKey = folderKey;
    if (changed) saveSelectedFolder();
    rebuildFolderPicker();
    rebuildFilter();
    const int row = programList.getSelectedRow();
    programList.scrollToEnsureRowIsOnscreen(row >= 0 ? row : 0);
    followActiveRow();   // its row is on screen now: shown, a later index does not scroll to it again
}

void AVDexedPlayView::showFolderPicker() {
    auto& language = AVDexedLanguageService::getInstance();
    const auto root = libraryRoot();
    PopupMenu menu;
    const auto icon = [](bool favorite) {
        return avDrawableFromSvg(favorite ? BinaryData::AVDexed_FavoriteIcon_svg : BinaryData::AVDexed_FolderIcon_svg,
                                 favorite ? BinaryData::AVDexed_FavoriteIcon_svgSize : BinaryData::AVDexed_FolderIcon_svgSize);
    };
    menu.addItem(1, language.text("allSounds"), true, selectedFolderKey == "all", icon(false));
    menu.addItem(2, language.text("favorites"), true, selectedFolderKey == "favorites", icon(true));
    const auto folders = AVDexedLibrary::listFolders(root, true);
    const bool loose = hasLooseSounds();
    if (! folders.isEmpty() || loose) menu.addSeparator();
    StringArray keys;
    const auto selectedFolder = selectedFolderKey.fromFirstOccurrenceOf("folder:", false, false);
    for (const auto& folder : folders) {
        const auto relative = avRelativeFolder(folder, root);
        const auto key = "folder:" + relative;
        menu.addItem(100 + keys.size(), folderLabel(key), true,
                     selectedFolderKey.startsWith("folder:") && avSameFolder(relative, selectedFolder), icon(false));
        keys.add(key);
    }
    if (loose) menu.addItem(3, language.text("looseSounds"), true, selectedFolderKey == "loose", icon(false));
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    menu.setLookAndFeel(&getLookAndFeel());  // menus never inherit it from their target
    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&folderPickerButton),
                       [safeThis, keys](int result) {
        if (safeThis == nullptr || result == 0)
            return;
        if (result == 1) safeThis->setSelectedFolder("all");
        else if (result == 2) safeThis->setSelectedFolder("favorites");
        else if (result == 3) safeThis->setSelectedFolder("loose");
        else if (isPositiveAndBelow(result - 100, keys.size()))
            safeThis->setSelectedFolder(keys[result - 100]);
    });
}

void AVDexedPlayView::showFolderActions() {
    auto& language = AVDexedLanguageService::getInstance();
    const auto root = libraryRoot();
    const auto folder = getSelectedLibraryFolder();
    // "Todos", "Favoritos" and "Sin carpeta" are views, and packs stay as installed.
    const bool editable = folder != File() && ! AVDexedLibrary::isReadOnly(folder, root);
    PopupMenu menu;
    menu.addItem(1, avEllipsis(language.text("newFolder")));
    menu.addItem(2, avEllipsis(language.text("renameFolder")), editable);
    menu.addItem(3, avEllipsis(language.text("deleteFolder")), editable);
    menu.addSeparator();
    menu.addItem(4, avRevealText());
    menu.addItem(5, language.text("reindexLibrary"));
    menu.addItem(6, language.text("restoreFactory"));
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    menu.setLookAndFeel(&getLookAndFeel());  // menus never inherit it from their target
    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&folderActionsButton),
                       [safeThis, folder, root](int result) {
        if (safeThis == nullptr)
            return;
        if (result == 1) safeThis->promptNewFolder(nullptr);
        else if (result == 2) safeThis->promptRenameFolder();
        else if (result == 3) safeThis->promptDeleteFolder();
        else if (result == 4) (folder != File() && folder.isDirectory() ? folder : root).revealToUser();
        else if (result == 5) safeThis->refreshLibrary();
        else if (result == 6) safeThis->restoreFactorySounds();
    });
}

void AVDexedPlayView::promptNewFolder(std::shared_ptr<PresetEntry> soundToCopy) {
    auto& language = AVDexedLanguageService::getInstance();
    const auto root = libraryRoot();
    AVDexedPromptOverlay::Options options;
    options.title = language.text("newFolder");
    options.message = soundToCopy != nullptr
        ? language.format("newFolderWithSoundPrompt", {{"file", fileOf(*soundToCopy).getFileName()}})
        : language.text("folderNamePrompt");
    options.hasEditor = true;
    options.placeholder = language.text("folderName");
    options.maxLength = 48;
    options.confirmText = language.text("createAction");
    options.cancelText = language.text("cancel");
    options.validate = [root](const String& text) {
        auto& lang = AVDexedLanguageService::getInstance();
        const auto name = AVDexedLibrary::sanitizeFolderName(text);
        if (name.isEmpty()) return lang.text("folderNameInvalid");
        if (AVDexedLibrary::folderNamed(root, name).exists()) return lang.text("folderExists");
        return String();
    };
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    showPrompt(std::move(options), [safeThis, root, soundToCopy](bool confirmed, const String& text) {
        if (safeThis == nullptr || ! confirmed) return;
        const auto name = AVDexedLibrary::sanitizeFolderName(text);
        if (name.isEmpty()) return;
        const auto folder = AVDexedLibrary::folderNamed(root, name);
        auto& lang = AVDexedLanguageService::getInstance();
        // Made meanwhile (Finder, another AV Dexed): it may be a read-only pack,
        // so nothing is copied into it.
        if (folder.exists()) {
            safeThis->notify(lang.text("folderExists"));
            safeThis->libraryChanged();
            return;
        }
        if (! folder.createDirectory().wasOk()) {
            safeThis->notify(lang.format("folderCreateFailed", {{"folder", name}}));
            return;
        }
        safeThis->setSelectedFolder("folder:" + folder.getFileName());
        if (soundToCopy != nullptr) safeThis->copyToFolder(*soundToCopy, folder);
        else safeThis->libraryChanged();
    });
}

void AVDexedPlayView::promptRenameFolder() {
    const auto root = libraryRoot();
    const auto folder = getSelectedLibraryFolder();
    if (folder == File() || AVDexedLibrary::isReadOnly(folder, root)) return;
    auto& language = AVDexedLanguageService::getInstance();
    AVDexedPromptOverlay::Options options;
    options.title = language.text("renameFolder");
    options.message = language.text("folderNamePrompt");
    options.hasEditor = true;
    options.editorText = folder.getFileName();
    options.placeholder = language.text("folderName");
    options.maxLength = 48;
    options.confirmText = language.text("renameAction");
    options.cancelText = language.text("cancel");
    options.validate = [folder](const String& text) {
        auto& lang = AVDexedLanguageService::getInstance();
        const auto name = AVDexedLibrary::sanitizeFolderName(text);
        if (name.isEmpty()) return lang.text("folderNameInvalid");
        const auto target = folder.getSiblingFile(name);
        // A change of case only is the same folder; anything else may not exist yet.
        if (target != folder && target.exists()) return lang.text("folderExists");
        return String();
    };
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    showPrompt(std::move(options), [safeThis, root, folder](bool confirmed, const String& text) {
        if (safeThis == nullptr || ! confirmed) return;
        auto& lang = AVDexedLanguageService::getInstance();
        File renamed;
        const auto result = AVDexedLibrary::renameFolder(folder, text, root, &renamed);
        if (result != AVDexedLibrary::RenameResult::renamed) {
            safeThis->notify(result == AVDexedLibrary::RenameResult::alreadyExists ? lang.text("folderExists")
                             : result == AVDexedLibrary::RenameResult::invalidName ? lang.text("folderNameInvalid")
                             : lang.format("renameFailed", {{"name", folder.getFileName()}}));
            return;
        }
        safeThis->followMovedFile(folder, renamed);
        safeThis->selectedFolderKey = "folder:" + avRelativeFolder(renamed, root);
        safeThis->saveSelectedFolder();
        safeThis->rebuildFolderPicker();
        safeThis->libraryChanged();
        safeThis->refreshLabels();
    });
}

void AVDexedPlayView::promptDeleteFolder() {
    const auto root = libraryRoot();
    const auto folder = getSelectedLibraryFolder();
    if (folder == File() || AVDexedLibrary::isReadOnly(folder, root)) return;
    auto& language = AVDexedLanguageService::getInstance();
    // The real contents on disk, not only what the list shows.
    const auto contents = AVDexedLibrary::describeFolder(folder);
    const auto label = folderLabel(selectedFolderKey);
    AVDexedPromptOverlay::Options options;
    options.title = language.text("deleteFolder");
    options.message = language.format("deleteFolderMessage", {{"folder", label}, {"trash", avTrashName()},
                                                              {"files", String(contents.files())},
                                                              {"sounds", String(contents.sounds())}});
    if (contents.otherFiles > 0)
        options.message << "\n" << language.format("deleteFolderOther", {{"count", String(contents.otherFiles)}});
    options.confirmText = language.text("deleteAction");
    options.cancelText = language.text("cancel");
    options.returnConfirms = false;   // never one keystroke away
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    showPrompt(std::move(options), [safeThis, root, folder, label](bool confirmed, const String&) {
        if (safeThis == nullptr || ! confirmed) return;
        auto& lang = AVDexedLanguageService::getInstance();
        const auto removed = AVDexedLibrary::removeToTrash(folder, root);
        using Status = AVDexedLibrary::RemoveResult::Status;
        if (removed.status != Status::trashed && removed.status != Status::keptHidden) {
            safeThis->notify(lang.format("deleteFailed", {{"name", label}}));
            return;
        }
        safeThis->forgetRemovedFile(folder, removed.hiddenCopy);
        const auto parent = AVDexedLibrary::folderOf(folder, root);
        safeThis->selectedFolderKey = parent.isNotEmpty() ? "folder:" + parent : String("all");
        safeThis->saveSelectedFolder();
        safeThis->rebuildFolderPicker();
        safeThis->notify(removed.status == Status::trashed ? lang.format("movedToTrash", {{"name", label}, {"trash", avTrashName()}})
                                                           : lang.format("keptHidden", {{"name", label}}));
        safeThis->libraryChanged();
    });
}

void AVDexedPlayView::promptRenameSound(const PresetEntry& entry) {
    const auto root = libraryRoot();
    const auto file = entry.file;
    if (! entry.isUser || entry.readOnly || entry.transient || ! file.existsAsFile()) return;
    auto& language = AVDexedLanguageService::getInstance();
    AVDexedPromptOverlay::Options options;
    options.title = language.text("renameSoundTitle");
    options.message = language.text("soundNamePrompt");
    options.hasEditor = true;
    options.editorText = entry.name;
    options.placeholder = language.text("soundName");
    options.maxLength = 48;
    options.confirmText = language.text("renameAction");
    options.cancelText = language.text("cancel");
    options.validate = [file](const String& text) {
        auto& lang = AVDexedLanguageService::getInstance();
        const auto stem = AVDexedLibrary::sanitizeFolderName(text);
        if (stem.isEmpty()) return lang.text("soundNameInvalid");
        const auto target = file.getSiblingFile(stem + file.getFileExtension());
        if (target != file && target.exists()) return lang.text("soundExists");
        return String();
    };
    const auto oldKey = entry.key;
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    showPrompt(std::move(options), [safeThis, root, file, oldKey](bool confirmed, const String& text) {
        if (safeThis == nullptr || ! confirmed) return;
        auto& lang = AVDexedLanguageService::getInstance();
        File renamed;
        if (AVDexedLibrary::renameSoundFile(file, text, root, &renamed) != AVDexedLibrary::RenameResult::renamed) {
            safeThis->notify(lang.format("renameFailed", {{"name", file.getFileNameWithoutExtension()}}));
            return;
        }
        // The list shows the name stored inside the preset: it changes with the file.
        const auto displayName = text.trim().substring(0, 48);
        avWriteNativePresetName(renamed, displayName);
        // This processor and those of the other Play views follow the file and its new name.
        safeThis->recordLibraryMove(file, renamed, displayName);
        // Presets without an id are known by their content, which now holds the new name.
        const auto newKey = safeThis->fingerprintFile(renamed);
        if (newKey != oldKey) safeThis->migratePresetKey(oldKey, newKey);
        safeThis->libraryChanged();
        safeThis->refreshLabels();
    });
}

void AVDexedPlayView::promptDeleteSound(const PresetEntry& entry) {
    const auto root = libraryRoot();
    const auto file = entry.file;
    if (! entry.isUser || entry.readOnly || entry.transient || ! file.existsAsFile()) return;
    auto& language = AVDexedLanguageService::getInstance();
    const auto name = entry.name;
    AVDexedPromptOverlay::Options options;
    options.title = language.text("deleteSoundTitle");
    options.message = language.format("deleteSoundMessage", {{"name", name}, {"trash", avTrashName()}});
    options.confirmText = language.text("deleteAction");
    options.cancelText = language.text("cancel");
    options.returnConfirms = false;
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    showPrompt(std::move(options), [safeThis, root, file, name](bool confirmed, const String&) {
        if (safeThis == nullptr || ! confirmed) return;
        auto& lang = AVDexedLanguageService::getInstance();
        const auto removed = AVDexedLibrary::removeToTrash(file, root);
        using Status = AVDexedLibrary::RemoveResult::Status;
        if (removed.status != Status::trashed && removed.status != Status::keptHidden) {
            safeThis->notify(lang.format("deleteFailed", {{"name", name}}));
            return;
        }
        safeThis->forgetRemovedFile(file, removed.hiddenCopy);
        safeThis->notify(removed.status == Status::trashed ? lang.format("movedToTrash", {{"name", name}, {"trash", avTrashName()}})
                                                           : lang.format("keptHidden", {{"name", name}}));
        safeThis->libraryChanged();
    });
}

void AVDexedPlayView::copyToFolder(const PresetEntry& entry, const File& folder) {
    // The file is copied as it is: the whole bank for a bank voice (with the
    // settings embedded in it), the .dexedpreset for a preset. Never overwrites.
    const auto source = fileOf(entry);
    if (! source.existsAsFile() || folder == File()) return;
    const auto result = entry.isUser ? AVDexedLibrary::importPreset(source, folder)
                                     : AVDexedLibrary::importBank(source, folder);
    auto& language = AVDexedLanguageService::getInstance();
    const auto label = folderLabel("folder:" + avRelativeFolder(folder, libraryRoot()));
    using Status = AVDexedLibrary::ImportResult::Status;
    if (result.status == Status::copied) notify(language.format("copiedToFolder", {{"folder", label}}));
    else if (result.status == Status::alreadyThere) notify(language.format("alreadyInFolder", {{"folder", label}}));
    else notify(language.format("copyFailed", {{"folder", label}}));
    libraryChanged();
}

void AVDexedPlayView::restoreFactorySounds() {
    // Only banks whose content is nowhere in the library come back (" 2" when
    // their name is taken by a different bank). Off the message thread.
    const auto root = libraryRoot();
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    libraryIndexPool.addJob([root, safeThis] {
        const int restored = AVDexedLibrary::installFactorySounds(root, BinaryData::builtin_pgm_zip,
                                                                  static_cast<size_t>(BinaryData::builtin_pgm_zipSize), true);
        MessageManager::callAsync([safeThis, restored] {
            if (safeThis == nullptr) return;
            auto& language = AVDexedLanguageService::getInstance();
            safeThis->notify(restored > 0 ? language.format("restoredFactory", {{"count", String(restored)}})
                                          : language.text("factoryComplete"));
            if (restored > 0) safeThis->libraryChanged();
        });
    });
}

void AVDexedPlayView::followMovedFile(const File& from, const File& to) {
    recordLibraryMove(from, to, {});
}

void AVDexedPlayView::recordLibraryMove(const File& from, const File& to, const String& presetName) {
    // What other views did before comes first, in its order; then this move,
    // which this view follows at once whatever the disk says now (a change of
    // case only leaves the old path "existing" on macOS and Windows).
    followLibraryMoves();
    avLibraryMoves().push_back({ from, to, presetName });
    AVFollowedMoves = avLibraryMoves().size();
    relinkProcessor(from, to, presetName, false);
    // The other views follow it when the change message reaches them.
}

bool AVDexedPlayView::followLibraryMoves() {
    const auto& moves = avLibraryMoves();
    bool relinked = false;
    while (AVFollowedMoves < moves.size()) {
        const auto move = moves[AVFollowedMoves++];
        relinked |= relinkProcessor(move.from, move.to, move.presetName, true);
    }
    return relinked;
}

bool AVDexedPlayView::relinkProcessor(const File& from, const File& to, const String& presetName, bool onlyWhenGone) {
    const auto moved = [&from, &to, onlyWhenGone](const File& file) {
        if (file.getFullPathName().isEmpty()) return File();
        // Another view's move, followed maybe much later (this window was
        // closed): a file made since at the old path is another file, and stays.
        // A change of case only is the same file on a disk that ignores case
        // (the old spelling still "exists"): followed, for its spelling and name.
        if (onlyWhenGone && file.exists() && to != from) return File();
        if (file == from) return to;
        return file.isAChildOf(from) ? to.getChildFile(file.getRelativePathFrom(from)) : File();
    };
    bool relinked = false;
    const auto bankFile = moved(processor.activeFileCartridge);
    if (bankFile != File()) {
        const Cartridge bank = processor.currentCart;   // a copy: setActiveBank reloads currentCart
        processor.setActiveBank(bank, bankFile);
        relinked = true;
    }
    const auto presetPath = processor.getAVPresetPath();
    const auto presetFile = moved(presetPath);
    if (presetFile != File()) {
        const bool wasModified = processor.isAVPresetModified();
        // A renamed preset shows its new name (it is written inside the file too).
        const auto name = presetName.isNotEmpty() && presetPath == from ? presetName : processor.getAVPresetName();
        processor.setAVPresetContext(name, processor.getAVPresetId(), presetFile);
        if (! wasModified) processor.captureAVPresetBaseline();
        relinked = true;
    }
    return relinked;
}

bool AVDexedPlayView::isRemovedFile(const File& file) {
    return file.isAChildOf(libraryRoot().getChildFile(AVDexedLibrary::deletedFolderName));
}

void AVDexedPlayView::forgetRemovedFile(const File& removed, const File& hiddenCopy) {
    // The sound keeps playing, but its file left the library. The processor
    // points at the hidden copy in .deleted (gone once the trash took it):
    // "Guardar" never writes there, nor into another file that later takes the
    // old name, and goes to "Guardar como…". Clearing a preset's path instead
    // would turn it into a voice of the bank its session came from, and
    // "Guardar" would then write it into that bank's slot.
    // Every other Play view of this process does the same with its processor:
    // left to guess by content, it would take an identical copy elsewhere.
    const auto hidden = hiddenCopy != File() ? hiddenCopy
                                             : libraryRoot().getChildFile(AVDexedLibrary::deletedFolderName).getChildFile(removed.getFileName());
    recordLibraryMove(removed, hidden, {});
    updateBank();
}

void AVDexedPlayView::libraryChanged() {
    reindexLibrary();
    avLibraryChanges.sendChangeMessage();
}

void AVDexedPlayView::pollLibrary() {
    if (! isShowing()) return;
    // Favourites starred in another AV Dexed (another DAW, the standalone).
    const auto preferencesTime = DexedAudioProcessor::getPreferencesFile().getLastModificationTime();
    if (preferencesTime != AVPreferencesTime) {
        loadFavorites();
        rebuildFilter();
        refreshLabels();
    }
    checkLibrarySignature();
}

void AVDexedPlayView::checkLibrarySignature() {
    // A poll never cancels an index in progress (it would never finish while
    // Finder keeps copying); the next tick looks again.
    if (libraryIndexing || AVSignaturePending) return;
    AVSignaturePending = true;
    const auto root = libraryRoot();
    Component::SafePointer<AVDexedPlayView> safeThis(this);
    libraryIndexPool.addJob([root, safeThis] {
        auto signature = AVDexedLibrary::librarySignature(root);
        MessageManager::callAsync([safeThis, signature] {
            if (safeThis == nullptr) return;
            safeThis->AVSignaturePending = false;
            if (! safeThis->libraryIndexing && signature != safeThis->AVPublishedSignature)
                safeThis->reindexLibrary();
        });
    });
}

void AVDexedPlayView::showPrompt(AVDexedPromptOverlay::Options options, AVDexedPromptOverlay::Result result) {
    if (onPromptRequested) onPromptRequested(std::move(options), std::move(result));
}

void AVDexedPlayView::notify(const String& message) {
    if (onNotice && message.isNotEmpty()) onNotice(message);
}

//==============================================================================
void AVDexedPlayView::toggleFavorite() {
    toggleFavoriteKey(activePresetKey());
}

void AVDexedPlayView::toggleFavoriteKey(const String& key) {
    if (key.isEmpty()) return;
    // Decided on what the star showed; written as this one change.
    if (favoriteKeys.contains(key)) updateFavorites({}, StringArray(key));
    else updateFavorites(StringArray(key), {});
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
    PropertiesFile preferences(DexedAudioProcessor::getPreferencesFile(), AVDexedLibrary::preferencesOptions());
    favoriteKeys = StringArray::fromLines(preferences.getValue("favoritePresetIds"));
    favoriteKeys.removeEmptyStrings();
    AVPreferencesTime = DexedAudioProcessor::getPreferencesFile().getLastModificationTime();
}

void AVDexedPlayView::updateFavorites(const StringArray& add, const StringArray& remove) {
    StringArray stored;
    avEditPreferences([&add, &remove, &stored](PropertiesFile& preferences) {
        stored = StringArray::fromLines(preferences.getValue("favoritePresetIds"));
        stored.removeEmptyStrings();
        for (const auto& key : remove) stored.removeString(key);
        for (const auto& key : add) stored.addIfNotAlreadyThere(key);
        preferences.setValue("favoritePresetIds", stored.joinIntoString("\n"));
    });
    favoriteKeys = stored;
    AVPreferencesTime = DexedAudioProcessor::getPreferencesFile().getLastModificationTime();
    avBrowserPreferenceChanges.sendChangeMessage();
}
