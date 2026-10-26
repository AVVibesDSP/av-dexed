// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../PluginProcessor.h"
#include "AVDexedTheme.h"
#include "AVDexedLanguage.h"
#include "DXLookNFeel.h"
#include "component/AlgoDisplay.h"
#include "AVDexedPromptOverlay.h"
#include "AVDexedPixelCache.h"
#include "AVDexedAnimationClock.h"
#include <array>
#include <atomic>
#include <map>

class AVDexedActionIconButton final : public juce::TextButton {
public:
    enum class Glyph { previous, next, favorite };
    explicit AVDexedActionIconButton(Glyph glyph);
    void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;
    /** Right-click (Ctrl-click on a Mac): when set, it opens this menu instead
        of clicking the button (a JUCE button clicks with any mouse button). */
    std::function<void()> onPopupMenu;
    /** Help read each time it shows (it names a learned CC); else the tooltip. */
    std::function<juce::String()> AVTooltipSource;
    juce::String getTooltip() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;

private:
    bool AVPopupGesture = false;
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

/** "Importar banco": a full-width pill (the 12% capsule that lifts on hover)
    with an SVG "+" and the label. The "+" keeps the ocean gradient at rest (a
    little dimmer) like the folder icon above it; the letters turn ocean on hover. */
class AVDexedImportButton final : public juce::Button {
public:
    AVDexedImportButton();
    void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;

private:
    std::unique_ptr<juce::Drawable> plusIcon;
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
    /** Right-click on ‹ (0) or › (1): the Play view shows its MIDI learn menu. */
    std::function<void(int step)> onStepMenu;
    /** The CC learned for ‹ (0) or › (1), or -1: their help names it. */
    std::function<int(int step)> AVStepController;

private:
    class NameButton;
    juce::String stepHelp(int step) const;
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
class AVDexedValueTip final : public juce::Component {
public:
    AVDexedValueTip();
    /** Shows the value above `target`; a click on the tip lets the user type one. */
    void show(juce::Slider& target, const juce::String& text);
    void release();
    void paint(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& event) override;

private:
    bool step(double now);
    void beginEditing();
    void finishEditing(bool commit, bool returnFocusToKnob = true);
    juce::String text;
    juce::Component::SafePointer<juce::Slider> slider;
    std::unique_ptr<juce::TextEditor> editor;
    float opacity = 0.0f;
    double holdUntil = 0.0, lastStep = 0.0;
    bool held = false;
    AVDexedAnimation animation { *this, [this](double now) { return step(now); } };
};

/** Search field whose capsule (painted by the Play view) reacts to focus and
    hover; the view repaints the whole capsule, not just the text area. */
class AVDexedSearchField final : public juce::TextEditor {
public:
    std::function<void()> onLookChanged;
    /** Up (-1) / Down (+1) typed in the field: the field is one line, so they
        step through the sounds instead of moving the caret. */
    std::function<void(int direction)> onStep;
    bool keyPressed(const juce::KeyPress& key) override;
    void focusGained(FocusChangeType cause) override { juce::TextEditor::focusGained(cause); changed(); }
    void focusLost(FocusChangeType cause) override { juce::TextEditor::focusLost(cause); changed(); }
    void mouseEnter(const juce::MouseEvent& event) override { juce::TextEditor::mouseEnter(event); changed(); }
    void mouseExit(const juce::MouseEvent& event) override { juce::TextEditor::mouseExit(event); changed(); }

private:
    void changed() { if (onLookChanged) onLookChanged(); }
};

/** Preset list where the keys load the sound they reach right away (no Enter
    needed): Up/Down step through the sounds as the arrows next to the name do
    (onStep), Page Up/Down and Home/End jump (onKeyboardSelection). Left/Right
    do nothing here, and arrows never turn the focus rings on. */
class AVDexedPresetList final : public juce::ListBox {
public:
    std::function<void(int)> onKeyboardSelection;
    std::function<void(int direction)> onStep;
    bool keyPressed(const juce::KeyPress& key) override;
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
    /** Arrows that reach the view from a child (its buttons): Up/Down step
        through the sounds (navigatePreset), Left/Right do nothing. */
    bool keyPressed(const juce::KeyPress& key) override;
    /** Follows the processor after a sound change: active row, transient rows
        for a bank or preset outside the library, labels. Cheap: never re-reads
        the library and never writes the preferences (it runs on every change). */
    void updateBank();
    /** Selects a real library folder by its path relative to the library root
        ("Pianos", "Pianos/Rhodes") and re-reads the library. */
    void showLibraryFolder(const juce::String& relativeFolder);
    /** The real folder selected in the picker, or File() for All, Favorites or
        "Sin carpeta" (loose files in the root). */
    juce::File getSelectedLibraryFolder() const;
    /** Re-reads the library from disk (after an import, a save, a change in Finder). */
    void refreshLibrary();
    /** After "Guardar como": once the index lists `file`, the browser shows it,
        opening its folder when the one shown does not hold it. */
    void revealWhenIndexed(const juce::File& file);
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
    /** The first reading of the library is still running and nothing is listed yet. */
    bool isWaitingForFirstIndex() const noexcept { return libraryIndexing && visibleEntries.empty(); }
    /** Owned here, shown in the editor's top bar on both tabs. */
    AVDexedPresetNavigator& getPresetNavigator() noexcept { return presetNavigator; }
    void setOperatorLevels(const std::array<float, 6>& levels);
    std::function<void(int)> onOperatorSelected;
    std::function<void()> onSaveRequested;
    std::function<void()> onSaveAsRequested;
    std::function<void(const juce::File&)> onPresetLoadRequested;
    std::function<void(int, bool)> onProgramSelectionRequested;
    std::function<void()> onImportBankRequested;
    /** Opens an in-editor dialog (the editor owns it). */
    std::function<void(AVDexedPromptOverlay::Options, AVDexedPromptOverlay::Result)> onPromptRequested;
    /** A short confirmation for the footer ("Copiado a «Pianos»"). */
    std::function<void(const juce::String&)> onNotice;
    /** Previous (-1) or next (+1) sound of the list shown, as the arrows next to
        the sound name do (the arrow keys and learned MIDI controls use it too). */
    void navigatePreset(int direction);
    /** "Aprender MIDI" on the previous (0) / next (1) arrow: the editor shows
        the waiting dialog and starts the processor's learn. */
    std::function<void(int step)> onPresetStepLearnRequested;
    /** Each time the library has been read: its .syx banks (none once they
        are all sound files). */
    std::function<void(const juce::Array<juce::File>& banks)> onLibraryListed;
    /** "Organizar" › convert the library's banks into sounds / undo that. */
    std::function<void()> onConvertLibrary, onUndoLibraryConversion;
    /** "Organizar" › export: the sound files of the folder shown, in its order, and its name. */
    std::function<void(const juce::Array<juce::File>& sounds, const juce::String& folderName)> onExportSounds;
    /** Favourites that follow their sounds to new keys (a bank voice that
        became a sound file, and back): old key → new key. */
    void replaceFavoriteKeys(const std::map<juce::String, juce::String>& oldToNew);
    /** How a bank or folder name reads beside its sounds and in the centre: an
        AV expansion ("AV_DEXED_EXPANSION_Pianos_01", any case) drops its first
        three '_' tokens ("Pianos_01"). Display only: search, files and the
        folder picker keep the full name. */
    static juce::String AVDisplaySourceName(const juce::String& name);

private:
    struct PresetEntry;
    friend bool AVRenderUIEvidence(const juce::File&);
    friend bool AVRunLibraryQA(const juce::File&);
    friend int AVRunIndexBench(const juce::File&, bool);
    friend bool runAVDexedDropTests();
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& event) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void returnKeyPressed(int lastRowSelected) override;
    void rebuildFilter();
    void requestEntryLoad(int row);
    void selectProgram(int row);
    juce::String activePresetKey() const;
    /** The row that plays: bank voices by (bank file, slot), presets by file. A
        bank without file plays as its transient row, or as the library copy
        that stands in for it (AVStandInBankFile). */
    bool isActiveEntry(const PresetEntry& entry) const;
    void selectActivePreset();
    /** Scrolls once to the row of a sound that just started playing (a program
        change, previous/next, a load, the window opening); a mere refresh
        leaves the list where it is. Waits until that row exists and is
        selected, and until the list has its height. */
    void followActiveRow();
    void publishLibrary(std::vector<PresetEntry> entries, unsigned generation, juce::String signature,
                        juce::StringPairArray carriedFavorites);
    void setSelectedFolder(const juce::String& folderKey);
    void rebuildFolderPicker();
    void showFolderPicker();
    /** The picker's folders: "User Presets" (where new sounds are saved) and
        its subfolders first, `pinned` of them, beside "Todos" and "Favoritos";
        then the rest, as listFolders() orders them. */
    juce::Array<juce::File> pickerFolders(int& pinned) const;
    void showFolderActions();
    void reindexLibrary();
    juce::String fingerprintFile(const juce::File& file) const;
    void toggleFavorite();
    void toggleFavoriteKey(const juce::String& key);
    void showValueTip(juce::Slider& slider);
    /** Clears the search and scrolls to the sound that plays, keeping the folder
        when it holds that sound (the top-bar name does this). */
    void revealActivePreset();
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void loadFavorites();
    /** Re-reads the favourites on disk and applies only these changes, holding
        the preferences lock: two AV Dexed (two DAWs) never undo each other. */
    void updateFavorites(const juce::StringArray& add, const juce::StringArray& remove);
    void showEffectSettings(int kind, juce::Button& anchor);
    /** Right-click on ‹ (0) / › (1): "Aprender MIDI…" and, with a CC learned,
        "Quitar MIDI (CC n)". */
    void showAVPresetStepMenu(int step);
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    // Real folders (the library on disk).
    static juce::File libraryRoot();
    static juce::File fileOf(const PresetEntry& entry);
    bool folderKeyExists(const juce::String& folderKey) const;
    juce::String folderLabel(const juce::String& folderKey) const;
    bool hasLooseSounds() const;
    void loadBrowserPreferences();
    void saveSelectedFolder();
    void migrateLegacyFolders();
    /** Keys older versions gave this sound (paths, whole-file hashes, "stock:NN"). */
    static juce::StringArray legacyKeysOf(const PresetEntry& entry);
    void syncTransientEntries();
    /** What plays, as a path and slot (to follow a change of sound to its row). */
    juce::String activeIdentity() const;
    void pollLibrary();
    /** Re-reads the library only when its signature (computed off the message
        thread) differs from the one last published. */
    void checkLibrarySignature();
    /** After an own change on disk: every Play view of this process re-reads. */
    void libraryChanged();
    void promptNewFolder(std::shared_ptr<PresetEntry> soundToCopy);
    void promptRenameFolder();
    void promptDeleteFolder();
    void promptRenameSound(const PresetEntry& entry);
    void promptDeleteSound(const PresetEntry& entry);
    void copyToFolder(const PresetEntry& entry, const juce::File& folder);
    void restoreFactorySounds();
    /** Keeps the processor on its bank or preset after AV Dexed moved it (and
        every other Play view of this process on theirs). */
    void followMovedFile(const juce::File& from, const juce::File& to);
    /** After a removal: the processor follows its bank or preset to `hiddenCopy`
        (inside .deleted), so "Guardar" goes to "Guardar como…". Every other
        Play view of this process does the same with its own processor. */
    void forgetRemovedFile(const juce::File& removed, const juce::File& hiddenCopy);
    /** Records a move or removal this view made (`presetName`: a renamed
        preset's new name) for every Play view of this process, and follows it
        here at once. */
    void recordLibraryMove(const juce::File& from, const juce::File& to, const juce::String& presetName);
    /** Follows the moves and removals other Play views recorded since this
        processor last looked (also while its window was closed). True when
        the processor now names another file. */
    bool followLibraryMoves();
    /** Points the processor's bank and preset at `to` when they are `from` or
        inside it. With `onlyWhenGone`, a file that still exists at the old
        path (made there since) stays as it is. True when something changed. */
    bool relinkProcessor(const juce::File& from, const juce::File& to, const juce::String& presetName,
                         bool onlyWhenGone);
    /** Inside the root's hidden .deleted folder (removed, maybe already in the trash). */
    static bool isRemovedFile(const juce::File& file);
    void showPrompt(AVDexedPromptOverlay::Options options, AVDexedPromptOverlay::Result result);
    void notify(const juce::String& message);

    struct AVPresetBank {
        Cartridge cartridge;
        juce::File file;
        juce::String fingerprint;
        juce::String legacyFingerprint;
        juce::StringArray legacyIdentityPrefixes;
        juce::int64 fileSize = 0;              // what the in-memory index cache compares
        juce::int64 modificationTime = 0;
    };

    struct PresetEntry {
        int program = -1;
        juce::File file;                       // the .dexedpreset (bank voices: bank->file)
        juce::String name;
        juce::String source;
        juce::String key;                      // content identity (favourites)
        bool isUser = false;
        bool isStock = false;                  // inside a read-only pack (factory)
        std::shared_ptr<AVPresetBank> bank;
        juce::int64 indexedFileSize = 0;
        juce::int64 indexedModificationTime = 0;
        juce::String folder;                   // relative to the root with '/', "" = loose
        bool readOnly = false;
        bool transient = false;                // the active sound, outside the library
        int order = -1;                        // a converted bank voice's place in its folder (-1: none)
    };

    DexedAudioProcessor& processor;
    AVDexedPlayLookAndFeel playLookAndFeel;
    juce::Label browserHelp;
    juce::Label emptyLibraryLabel;
    AVDexedSearchField searchField;
    AVDexedFolderPickerButton folderPickerButton;
    juce::TextButton folderActionsButton { "Manage" };
    AVDexedImportButton importBankButton;
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
    /** On a focused Play knob, Up/Down step through the sounds (not the value). */
    std::unique_ptr<juce::KeyListener> AVKnobKeys;
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
    std::vector<PresetEntry> transientEntries;
    std::vector<PresetEntry> visibleEntries;
    std::map<int, PresetEntry> requestedEntries;
    int nextRequestToken = 1;
    juce::ThreadPool libraryIndexPool { 1 };
    std::shared_ptr<std::atomic<bool>> libraryIndexCancelled;
    unsigned libraryGeneration = 0;
    bool libraryIndexing = false;
    std::unique_ptr<juce::Component> effectSettingsWindow;
    juce::StringArray favoriteKeys;
    /** "all", "favorites", "loose" (Sin carpeta) or "folder:<relative path>". */
    juce::String selectedFolderKey { "all" };
    juce::String AVLegacySelectedFolder;       // a 0.1.0 virtual folder id, until it is migrated
    juce::String AVPublishedSignature;
    juce::String AVTransientIdentity;
    juce::String AVShownActiveIdentity;
    /** The library bank shown as the active one while the processor plays the
        same bank without a file (a SysEx dump, the built-in Dexed_01). */
    juce::File AVStandInBankFile;
    juce::File AVRevealFile;   // revealWhenIndexed(): waiting for the index
    /** How many of the process-wide library moves this view has followed. */
    size_t AVFollowedMoves = 0;
    juce::Time AVPreferencesTime;
    bool AVSignaturePending = false;
    bool AVLegacyFoldersChecked = false;
    std::unique_ptr<juce::Drawable> favoriteRowIcon;
    std::unique_ptr<juce::Drawable> favoriteRowIconOff;
    std::unique_ptr<juce::Drawable> folderRowIcon;
    bool modifiedCurrent = false;
    int hoveredRow = -1;
    juce::Rectangle<int> browserCard;
    /** The card's header: "Sonidos • 1088", painted centred in one weight. */
    juce::Rectangle<int> AVHeaderBounds;
    juce::Rectangle<int> searchBounds;
    juce::Rectangle<int> heroBounds;
    /** The canvas, the ocean light and the browser card: they change only
        with the size, so they are an image at the screen's pixels. */
    AVDexedPixelCache backgroundLayer;
    juce::Rectangle<int> centreTitleBounds;
    /** Light poll (2 s) for changes made in Finder or by another AV Dexed. */
    juce::TimedCallback AVLibraryPoll { [this] { pollLibrary(); } };
};
