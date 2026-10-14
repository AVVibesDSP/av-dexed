// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../PluginProcessor.h"
#include "AVDexedTheme.h"
#include "AVDexedLanguage.h"
#include "DXLookNFeel.h"
#include "component/AlgoDisplay.h"
#include <array>
#include <atomic>
#include <map>

class AVDexedActionIconButton final : public juce::TextButton {
public:
    enum class Glyph { previous, next, favorite };
    explicit AVDexedActionIconButton(Glyph glyph);
    void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;

private:
    Glyph glyph;
    std::unique_ptr<juce::Drawable> AVIcon;
    std::unique_ptr<juce::Drawable> AVIconActive;
};

class AVDexedFolderPickerButton final : public juce::TextButton {
public:
    AVDexedFolderPickerButton();
    void setSelection(const juce::String& name, bool isFavoritesFolder);
    void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;

private:
    std::unique_ptr<juce::Drawable> folderIcon;
    std::unique_ptr<juce::Drawable> favoriteIcon;
    juce::String folderName;
    bool favoritesSelected = false;
};

class AVDexedPlayLookAndFeel final : public DXLookNFeel {
public:
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                          juce::Slider& slider) override;
};

/** Top-bar preset navigation: ‹ name ☆ › over a one-line status. The star sits
    right after the name; it is text-coloured and turns ocean only when the
    preset is a favourite. */
class AVDexedPresetNavigator final : public juce::Component {
public:
    AVDexedPresetNavigator();
    ~AVDexedPresetNavigator() override;
    void setPreset(const juce::String& name, const juce::String& status, bool modified,
                   bool favorite, bool favoriteAvailable);
    void applyLanguage();
    void paint(juce::Graphics& g) override;
    void resized() override;
    std::function<void()> onPrevious, onNext, onFavorite, onName;

private:
    class NameButton;
    AVDexedActionIconButton previous { AVDexedActionIconButton::Glyph::previous };
    AVDexedActionIconButton next { AVDexedActionIconButton::Glyph::next };
    AVDexedActionIconButton star { AVDexedActionIconButton::Glyph::favorite };
    std::unique_ptr<NameButton> name;
    juce::String presetName, statusText;
    bool presetModified = false;
    void layoutStar();
};

/** Glass value capsule shown above a knob while it is dragged (GOZO value tip):
    fades in over ~80 ms, stays 1.2 s after release, fades out over ~170 ms. */
class AVDexedValueTip final : public juce::Component, private juce::Timer {
public:
    AVDexedValueTip();
    /** Shows the value above `target`; a click on the tip lets the user type one. */
    void show(juce::Slider& target, const juce::String& text);
    void release();
    void paint(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& event) override;

private:
    void timerCallback() override;
    void beginEditing();
    void finishEditing(bool commit, bool returnFocusToKnob = true);
    juce::String text;
    juce::Component::SafePointer<juce::Slider> slider;
    std::unique_ptr<juce::TextEditor> editor;
    float opacity = 0.0f;
    int holdFrames = 0;
    bool held = false;
};

/** Search field whose capsule (painted by the Play view) reacts to focus and
    hover; the view repaints the whole capsule, not just the text area. */
class AVDexedSearchField final : public juce::TextEditor {
public:
    std::function<void()> onLookChanged;
    void focusGained(FocusChangeType cause) override { juce::TextEditor::focusGained(cause); changed(); }
    void focusLost(FocusChangeType cause) override { juce::TextEditor::focusLost(cause); changed(); }
    void mouseEnter(const juce::MouseEvent& event) override { juce::TextEditor::mouseEnter(event); changed(); }
    void mouseExit(const juce::MouseEvent& event) override { juce::TextEditor::mouseExit(event); changed(); }

private:
    void changed() { if (onLookChanged) onLookChanged(); }
};

/** Preset list where Up/Down (and Page Up/Down, Home/End) load the newly
    selected sound right away: no Enter needed. */
class AVDexedPresetList final : public juce::ListBox {
public:
    std::function<void(int)> onKeyboardSelection;
    bool keyPressed(const juce::KeyPress& key) override {
        using juce::KeyPress;
        const int before = getSelectedRow();
        const bool handled = juce::ListBox::keyPressed(key);
        const bool navigation = key.isKeyCode(KeyPress::upKey) || key.isKeyCode(KeyPress::downKey)
            || key.isKeyCode(KeyPress::pageUpKey) || key.isKeyCode(KeyPress::pageDownKey)
            || key.isKeyCode(KeyPress::homeKey) || key.isKeyCode(KeyPress::endKey);
        if (handled && navigation && getSelectedRow() != before && getSelectedRow() >= 0 && onKeyboardSelection)
            onKeyboardSelection(getSelectedRow());
        return handled;
    }
};

/** Caption under a Play knob. Effect captions are buttons: their letters turn
    ocean on hover and a small chevron says they open the effect's settings. */
class AVDexedKnobCaption final : public juce::Button {
public:
    explicit AVDexedKnobCaption(bool opensSettings);
    void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;

private:
    bool opensSettings;
};

class AVDexedPlayView final : public juce::Component, private juce::ListBoxModel,
                              private juce::ChangeListener {
public:
    explicit AVDexedPlayView(DexedAudioProcessor& processor);
    ~AVDexedPlayView() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;
    void updateBank();
    void refreshLabels();
    void setActiveUserPreset(const juce::File& file);
    void commitProgramSelection(int program, bool fromStock = false);
    void captureCurrentAsBaseline();
    juce::String getPresetKeyForFile(const juce::File& file) const { return fingerprintFile(file); }
    void migratePresetKey(const juce::String& oldKey, const juce::String& newKey);
    void carryAVBankMetadata(const Cartridge& previousBank, const Cartridge& storedBank);
    void applyLanguage();
    juce::File getActiveUserPreset() const;
    bool isModified() const noexcept { return modifiedCurrent; }
    /** Owned here, shown in the editor's top bar on both tabs. */
    AVDexedPresetNavigator& getPresetNavigator() noexcept { return presetNavigator; }
    void setOperatorLevels(const std::array<float, 6>& levels);
    std::function<void(int)> onOperatorSelected;
    std::function<void()> onSaveRequested;
    std::function<void()> onSaveAsRequested;
    std::function<void(const juce::File&)> onPresetLoadRequested;
    std::function<void(int, bool)> onProgramSelectionRequested;
    std::function<void()> onImportBankRequested;

private:
    struct PresetEntry;
    friend bool AVRenderUIEvidence(const juce::File&);
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& event) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void returnKeyPressed(int lastRowSelected) override;
    void rebuildFilter();
    void navigatePreset(int direction);
    void requestEntryLoad(int row);
    void selectProgram(int row);
    juce::String activePresetKey() const;
    void selectActivePreset();
    void publishLibrary(std::vector<PresetEntry> entries, unsigned generation);
    void setSelectedFolder(const juce::String& folderKey);
    void rebuildFolderPicker();
    void showFolderPicker();
    void showFolderActions();
    void editFolderName(const juce::String& id, const juce::String& initialName = {},
                        const juce::String& presetToAssign = {});
    void applyFolderName(const juce::String& id, const juce::String& name,
                         const juce::String& presetToAssign = {});
    void deleteSelectedFolder();
    void togglePresetFolder(const juce::String& folderId, const juce::String& presetKey);
    void loadFolderCatalog();
    void saveFolderCatalog();
    void reindexLibrary();
    struct PresetFolder;
    PresetFolder* findFolder(const juce::String& id);
    const PresetFolder* findFolder(const juce::String& id) const;
    juce::String fingerprintProgram(int program, bool fromStock = false) const;
    juce::String fingerprintFile(const juce::File& file) const;
    void toggleFavorite();
    void toggleFavoriteKey(const juce::String& key);
    void showValueTip(juce::Slider& slider);
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void loadFavorites();
    void saveFavorites();
    void showEffectSettings(int kind, juce::Button& anchor);
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    struct AVPresetBank {
        Cartridge cartridge;
        juce::File file;
        juce::String fingerprint;
        juce::String legacyFingerprint;
        juce::StringArray legacyIdentityPrefixes;
    };

    struct PresetEntry {
        int program = -1;
        juce::File file;
        juce::String name;
        juce::String source;
        juce::String key;
        bool isUser = false;
        bool isStock = false;
        std::shared_ptr<AVPresetBank> bank;
        juce::int64 indexedFileSize = 0;
        juce::int64 indexedModificationTime = 0;
    };

    struct PresetFolder {
        juce::String id;
        juce::String name;
        juce::StringArray presetKeys;
    };

    DexedAudioProcessor& processor;
    AVDexedPlayLookAndFeel playLookAndFeel;
    juce::Label browserTitle;
    juce::Label browserHelp;
    juce::Label browserCount;
    juce::Label emptyLibraryLabel;
    AVDexedSearchField searchField;
    AVDexedFolderPickerButton folderPickerButton;
    juce::TextButton folderActionsButton { "Manage" };
    juce::TextButton importBankButton { "Import bank" };
    juce::TextButton showActiveButton;
    AVDexedPresetList programList;
    juce::Label activeName;
    juce::Label sourceName;
    juce::Label modifiedLabel;
    AVDexedPresetNavigator presetNavigator;
    AVDexedValueTip valueTip;
    std::unique_ptr<AlgoDisplay> heroAlgorithm;
    juce::Label algorithmCaption;
    std::array<std::unique_ptr<AVDexedKnobCaption>, 6> knobCaptions;
    std::unique_ptr<juce::MouseListener> AVContextMenu;
    juce::Slider volumeSlider;
    juce::Slider panSlider;
    juce::Slider brightnessSlider;
    juce::Slider reverbSlider;
    juce::Slider delaySlider;
    juce::Slider chorusSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volumeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> panAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> brightnessAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> reverbAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> chorusAttachment;
    std::vector<PresetEntry> libraryEntries;
    std::vector<PresetEntry> visibleEntries;
    std::map<int, PresetEntry> requestedEntries;
    int nextRequestToken = 1;
    juce::StringArray knownBankPaths;
    juce::StringArray knownPresetPaths;
    juce::ThreadPool libraryIndexPool { 1 };
    std::shared_ptr<std::atomic<bool>> libraryIndexCancelled;
    unsigned libraryGeneration = 0;
    bool libraryIndexing = false;
    std::unique_ptr<juce::Component> effectSettingsWindow;
    juce::StringArray favoriteKeys;
    std::vector<PresetFolder> presetFolders;
    juce::String selectedFolderKey { "all" };
    std::unique_ptr<juce::Drawable> favoriteRowIcon;
    std::unique_ptr<juce::Drawable> favoriteRowIconOff;
    std::unique_ptr<juce::Drawable> folderRowIcon;
    bool modifiedCurrent = false;
    int hoveredRow = -1;
    AVDexedTheme::FocusKeyListener focusKeys;
    juce::Rectangle<int> browserCard;
    juce::Rectangle<int> searchBounds;
    juce::Rectangle<int> heroBounds;
    juce::Rectangle<int> centreTitleBounds;
};
