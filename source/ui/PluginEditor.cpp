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
#include "../core/AVDexedLibrary.h"
#include "../core/AVDexedSysex.h"
#include "../debugger/Logger.h"
#include "math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
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

/** A sound name as a file name ("" when nothing usable is left). */
juce::String avPresetFileStem(const juce::String& name) {
    auto stem = juce::File::createLegalFileName(name.trim()).trim();
    while (stem.startsWithChar('.')) stem = stem.substring(1).trimStart();
#if JUCE_WINDOWS
    // Windows refuses control characters (a pasted tab) in file names.
    juce::String cleaned;
    for (auto c : stem)
        if (c >= 32 && c != 127) cleaned << juce::String::charToString(c);
    stem = cleaned.trim();
    // Windows device names stay devices even with an extension ("AUX.avdexpreset").
    static const juce::StringArray devices { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6",
                                             "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
                                             "LPT7", "LPT8", "LPT9" };
    if (devices.contains(stem.upToFirstOccurrenceOf(".", false, false).trim(), true)) return {};
#endif
    return AVDexedLibrary::isIgnoredName(stem) ? juce::String() : stem;
}

#if JUCE_WINDOWS
// One pattern, so the Windows save dialog appends ".syx" (its filters ignore case).
constexpr const char* avBankSaveFilter = "*.syx";
#else
constexpr const char* avBankSaveFilter = "*.syx;*.SYX";
#endif

/** A library folder as the picker shows it ("Pianos / Rhodes"). */
juce::String avFolderDisplayName(const juce::File& folder, const juce::File& root) {
    return folder.getRelativePathFrom(root).replaceCharacter('\\', '/').replace("/", " / ");
}

/** Inside the library and visible there: not in a hidden folder of the root
    (.deleted, .migration-backup…), which the browser never shows. */
bool avIsInVisibleLibrary(const juce::File& file, const juce::File& root) {
    if (root == juce::File() || ! file.isAChildOf(root))
        return false;
    for (auto item = file; item != root && item.isAChildOf(root); item = item.getParentDirectory())
        if (AVDexedLibrary::isIgnoredName(item.getFileName()))
            return false;
    return true;
}

/** At most `limit` names, then "… (+N)": a dropped folder full of single-voice
    dumps must not grow a dialog taller than the editor. */
juce::String avNameList(const juce::StringArray& names, int limit = 6) {
    if (names.size() <= limit)
        return names.joinIntoString(", ");
    juce::StringArray shown(names);
    shown.removeRange(limit, shown.size() - limit);
    return shown.joinIntoString(", ") + juce::String::fromUTF8(", … (+") + juce::String(names.size() - limit) + ")";
}

bool avIsBankPath(const juce::String& path) { return path.endsWithIgnoreCase(".syx"); }
bool avIsPresetPath(const juce::String& path) {
    // A string test: callers also pass bare file names, which are not paths.
    return path.endsWithIgnoreCase(AVDexedLibrary::presetExtension) || path.endsWithIgnoreCase(AVDexedLibrary::legacyPresetExtension);
}
bool avIsTuningPath(const juce::String& path) { return path.endsWithIgnoreCase(".scl") || path.endsWithIgnoreCase(".kbm"); }
bool avIsLayersPath(const juce::String& path) { return path.endsWithIgnoreCase(DexedAudioProcessor::AVLayersExtension); }

/** Every dragged item is a single sound (.avdexpreset), not a bank or folder. */
bool avAllSounds(const juce::StringArray& files) {
    return ! files.isEmpty() && std::all_of(files.begin(), files.end(), [](const juce::String& path) {
        return avIsPresetPath(path) && ! juce::File(path).isDirectory();
    });
}

using AVSettingValues = std::map<juce::String, float>;

/** What a slot plays with, by the processor's rule (applyAVSoundSettings): its
    own entry while it still holds that voice, else the first entry with the
    same voice (moved or duplicated in the bank), else the defaults (empty). */
AVSettingValues avSettingsHeard(const AVDexedLibrary::BankSettings& settings, juce::uint64 voice, int slot) {
    if (const auto own = settings.find(slot); own != settings.end() && own->second.voice == voice)
        return own->second.values;
    for (const auto& [index, entry] : settings)
        if (entry.voice == voice)
            return entry.values;
    return {};
}

/** The same settings, within what a bank file keeps (6 decimals in its JSON). */
bool avSameSettings(const AVSettingValues& a, const AVSettingValues& b) {
    if (a.size() != b.size())
        return false;
    for (const auto& [id, value] : a) {
        const auto other = b.find(id);
        if (other == b.end() || std::abs(other->second - value) > 1.0e-4f * juce::jmax(1.0f, std::abs(value)))
            return false;
    }
    return true;
}

/** `settings` with `slot` of `bank` set to `values` (empty = defaults), every
    other slot playing exactly as before. Settings follow their voice by hash,
    so changing one entry can change what a duplicate of that voice finds: such
    a slot gets an entry of its own. The rule of "Guardar"
    (DexedAudioProcessor::saveAVSoundToBank); at most 32 pins. */
AVDexedLibrary::BankSettings avSettingsWithSlot(AVDexedLibrary::BankSettings settings, const Cartridge& bank,
                                               int slot, const AVSettingValues& values) {
    std::array<juce::uint64, 32> voices {};
    std::array<AVSettingValues, 32> heard;
    for (size_t index = 0; index < 32; ++index) {
        voices[index] = AVDexedLibrary::voiceHash(bank, static_cast<int>(index));
        heard[index] = avSettingsHeard(settings, voices[index], static_cast<int>(index));
    }
    const auto stored = static_cast<size_t>(slot);
    heard[stored] = values;
    if (values.empty())
        settings.erase(slot);
    else
        settings[slot] = { voices[stored], values };
    for (bool pinned = true; pinned;) {
        pinned = false;
        for (size_t index = 0; index < 32; ++index) {
            if (avSettingsHeard(settings, voices[index], static_cast<int>(index)) != heard[index]) {
                settings[static_cast<int>(index)] = { voices[index], heard[index] };
                pinned = true;
            }
        }
    }
    return settings;
}

/** The notice for a bank write that failed: AVWriteBankFile never rewrites an
    archive of several banks (64 KB or more) nor other data larger than a bank,
    and that is said as such (nothing changed), not as a disk error. */
juce::String avBankWriteError(const juce::File& file) {
    auto& language = AVDexedLanguageService::getInstance();
    Cartridge onDisk;
    const bool keptAsItIs = file.existsAsFile()
        && (file.getSize() >= 65535 || (file.getSize() > SYSEX_SIZE && ! AVDexedLibrary::readBank(file, onDisk)));
    return keptAsItIs ? language.format("bankNotRewritable", {{"bank", file.getFileName()}})
                      : language.format("writeError", {{"path", file.getFullPathName()}});
}

/** Every dragged item is a bank, a preset or a folder (whose sounds are added). */
bool avAllImportable(const juce::StringArray& files) {
    return ! files.isEmpty() && std::all_of(files.begin(), files.end(), [](const juce::String& path) {
        return avIsBankPath(path) || avIsPresetPath(path) || avIsLayersPath(path) || juce::File(path).isDirectory();
    });
}

struct AVImportFiles {
    juce::Array<juce::File> banks, presets;
    juce::String folderName;   // the first dropped folder: the suggested library folder
};

/** The banks and presets among `items`, looking inside folders. Hidden and
    system entries (.DS_Store, __MACOSX…) are skipped, and the walk is bounded so
    dropping a huge folder cannot hang the editor. */
AVImportFiles avCollectImportFiles(const juce::Array<juce::File>& items) {
    using namespace juce;
    AVImportFiles found;
    const auto add = [&found](const File& file) {
        const auto name = file.getFileName();
        if (AVDexedLibrary::isIgnoredName(name)) return;
        if (avIsBankPath(name)) found.banks.addIfNotAlreadyThere(file);
        else if (avIsPresetPath(name)) found.presets.addIfNotAlreadyThere(file);
    };
    for (const auto& item : items) {
        if (! item.isDirectory()) { add(item); continue; }
        if (found.folderName.isEmpty()) found.folderName = item.getFileName();
        int visited = 0;
        for (const auto& entry : RangedDirectoryIterator(item, true, "*", File::findFiles | File::ignoreHiddenFiles,
                                                         File::FollowSymlinks::noCycles)) {
            if (++visited > 20000) break;
            const auto file = entry.getFile();
            const auto parts = StringArray::fromTokens(file.getRelativePathFrom(item), "/\\", {});
            if (std::none_of(parts.begin(), parts.end(), [](const String& part) { return AVDexedLibrary::isIgnoredName(part); }))
                add(file);
        }
    }
    const auto byPath = [](const File& a, const File& b) {
        return a.getFullPathName().compareNatural(b.getFullPathName(), false) < 0;
    };
    std::sort(found.banks.begin(), found.banks.end(), byPath);
    std::sort(found.presets.begin(), found.presets.end(), byPath);
    return found;
}

/** The footer line for a drop of files the library already holds: their folder
    when they share one ("Ya estaba en «Pianos»"; "Sin carpeta" for the root),
    else the general line. */
juce::String avAlreadyInLibraryNotice(const AVImportFiles& found, const juce::File& root) {
    auto& language = AVDexedLanguageService::getInstance();
    juce::File folder;
    bool shared = true;
    int count = 0;
    for (const auto* list : { &found.banks, &found.presets })
        for (const auto& file : *list) {
            ++count;
            const auto parent = file.getParentDirectory();
            if (folder == juce::File()) folder = parent;
            else if (parent != folder) shared = false;
        }
    const bool many = count > 1;
    // Loose in the root: no folder to name ("Sin carpeta" would read as one).
    if (! shared || folder == juce::File() || folder == root)
        return language.text(many ? "alreadyInLibraryMany" : "alreadyInLibrary");
    return language.format(many ? "alreadyInFolderMany" : "alreadyInFolder", {{"folder", avFolderDisplayName(folder, root)}});
}

// Footer notice timing (ms): GOZO's toast fades, held for about 3 s in all.
constexpr double noticeFadeIn = 83.0, noticeHoldUntil = 2800.0, noticeFadeOut = 200.0, helpFadeIn = 117.0;

/** "AV Vibes" under the logo. */
juce::Font avIdentityFont() { return AVDexedTheme::font(13.0f, "SemiBold"); }

/** A sound arrow's name: 0 "Sonido anterior", 1 "Siguiente sonido". */
juce::String avPresetStepName(int step) {
    return AVDexedLanguageService::getInstance().text(step == 0 ? "previousSound" : "nextSound");
}

// The adoption of 0.1.0's external sounds runs once per process: idle until an
// editor queues it, started once its job runs. A job dropped unstarted (the
// editor closed at once, e.g. a host probing the view) leaves it idle again.
constexpr int avAdoptionIdle = 0, avAdoptionQueued = 1, avAdoptionStarted = 2;
std::atomic<int> avAdoptionState { avAdoptionIdle };
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
      layersView (*ownerFilter),
      cartManager(this), global(*ownerFilter, this) {
    ownerFilter->setAVEditorOpen(true);   // learned arrow presses now walk this editor's list
    processor = ownerFilter;
    auto& language = AVDexedLanguageService::getInstance();
    language.configure(DexedAudioProcessor::getPreferencesFile());
    language.addChangeListener(this);

    setExplicitFocusOrder(1);

    // The frame holds the keyboard when no control does. Without it, JUCE hands
    // the focus to the first control (the Play tab) whenever the window gets it,
    // the top bar is clicked or a view hides the focused knob; Return then
    // switched tabs, and an arrow key lit the tab's ring.
    frameComponent.setWantsKeyboardFocus(true);
    frameComponent.setTitle("AV Dexed");   // a screen reader names it when it holds the focus
    frameComponent.setBounds(0, 0, WINDOW_SIZE_X, WINDOW_SIZE_Y);
    addAndMakeVisible(&frameComponent);
    setLookAndFeel(&*lookAndFeel);
    background = AVDexedTheme::canvas;
    AVDexedTheme::lowPower = processor->lowPowerUI;
    setOpaque(true);   // paint fills it all: what lies under it is never painted

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

    for (auto* button : std::array<Button*, 4>{ &playTabButton, &advancedTabButton, &layersTabButton, &saveButton }) {
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
    // GOZO tabs switch on mouse down. A click leaves the keyboard where it was
    // (Tab still reaches them).
    for (auto* tab : { &playTabButton, &advancedTabButton, &layersTabButton }) {
        tab->getProperties().set("AVTab", true);
        tab->setTriggeredOnMouseDown(true);
        tab->setMouseClickGrabsKeyboardFocus(false);
    }
    advancedTabButton.onClick = [this] { showAVDexedPlayView(false); };
    layersTabButton.onClick = [this] { showAVView(AVView::layers); };
    saveButton.onClick = [this] {
        auto& language = AVDexedLanguageService::getInstance();
        PopupMenu menu;
        menu.addItem(1, language.text("save"));
        menu.addItem(2, language.text("saveAs"));
        Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
        menu.setLookAndFeel(&getLookAndFeel());
        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(&saveButton), [safeThis](int result) {
            if (safeThis == nullptr) return;
            if (result == 1) safeThis->AVSaveSound();
            else if (result == 2) safeThis->AVSaveSoundAs();
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

    playView.onSaveRequested = [this] { AVSaveSound(); };
    playView.onSaveAsRequested = [this] { AVSaveSoundAs(); };
    playView.onNotice = [this](const String& text) { showAVFooterNotice(text); };
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
        // Banks usually arrive in Downloads; the copy goes into the library.
        // (Whole folders are added by dragging them: a native chooser cannot
        // offer files and folders together on every platform.)
        auto start = File::getSpecialLocation(File::userHomeDirectory).getChildFile("Downloads");
        if (! start.isDirectory()) start = File::getSpecialLocation(File::userDocumentsDirectory);
        fileChooser = std::make_unique<FileChooser>(AVDexedLanguageService::getInstance().text("importBankTitle"), start,
                                                    String("*.syx;*.SYX;") + AVDexedLibrary::presetWildcards, true);
        Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
        fileChooser->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles
                                     | FileBrowserComponent::canSelectMultipleItems,
                                 [safeThis](const FileChooser& chooser) {
            if (safeThis != nullptr && ! chooser.getResults().isEmpty())
                safeThis->importBanks(chooser.getResults());
        });
    };
    playView.onPromptRequested = [this](AVDexedPromptOverlay::Options options, AVDexedPromptOverlay::Result result) {
        showAVPrompt(std::move(options), std::move(result));
    };
    playView.onPresetStepLearnRequested = [this](int step) { AVLearnPresetStep(step); };
    frameComponent.addAndMakeVisible(playView);
    frameComponent.addAndMakeVisible(playView.getPresetNavigator());
    frameComponent.addChildComponent(AVDropOverlay);
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
    layersView.onNotice = [this](const String& text) { showAVFooterNotice(text); };
    layersView.onReplaceSound = [this](std::function<void()> replacement) { requestPresetReplacement(std::move(replacement)); };
    frameComponent.addChildComponent(layersView);
    frameComponent.addChildComponent(AVPageFade);   // above the pages, under every overlay

    // OPERATORS
    for (int i=0;i<6;i++) {
        operators[i] = std::make_unique<OperatorEditor>(*processor, i);
        classicView.addAndMakeVisible(operators[i].get());
    }
    // One shared keyboard/state across both views.
    frameComponent.addAndMakeVisible(midiKeyboard);
    midiKeyboard.setScrollButtonsVisible(false);
    midiKeyboard.setWantsKeyboardFocus(true);
    // The computer keys play from C2, the lowest key on screen.
    midiKeyboard.setKeyPressBaseOctave(AVKeyboardAnchor / 12);
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
    // The raw 14-bit value (8192 at rest) is what MIDI carries; the footer and
    // screen readers show the bend itself: 0 at rest, −8192 … +8191.
    AVPitchWheel.textFromValueFunction = [](double value) {
        const int bend = roundToInt(value) - 8192;
        return bend > 0 ? "+" + String(bend) : bend < 0 ? String::fromUTF8("−") + String(-bend) : String("0");
    };
    AVPitchWheel.valueFromTextFunction = [](const String& text) {
        const auto bend = text.replace(String::fromUTF8("−"), "-").retainCharacters("0123456789-").getIntValue();
        return double(jlimit(0, 16383, bend + 8192));
    };
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
    // "AV Dexed" in Advanced: About inside Settings (blurred, fixed), not the
    // old desktop window.
    global.onAboutRequested = [this] { showAVSettingsPage(AVDexedSettingsPanel::AVPage::about); };

    resizeGrip.onResizeStart = [this] { resizeFactorAtStart = processor->getZoomFactor(); };
    resizeGrip.onResizeDrag = [this](Point<int> screenDelta) {
        const auto logicalHeight = processor->showKeyboard ? WINDOW_SIZE_Y : WINDOW_SIZE_Y - 112;
        const float width = static_cast<float>(WINDOW_SIZE_X);
        const float height = static_cast<float>(logicalHeight);
        const float dx = static_cast<float>(screenDelta.x);
        const float dy = static_cast<float>(screenDelta.y);
        const float step = (width * dx + height * dy) / (width * width + height * height);
        processor->setZoomFactor(jlimit(AVMinZoom, 1.6f, resizeFactorAtStart + step));
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
        processor->setZoomFactor(jlimit(AVMinZoom, 1.6f, jmin(processor->getZoomFactor(), fit)));
    }
    resetSize();
    showAVDexedPlayView(true, false);
    applyLanguage();
    addKeyListener(this);
    addMouseListener(&focusModality, true);
    Desktop::getInstance().addFocusChangeListener(this);
    if (JUCEApplicationBase::isStandaloneApp()) {
        AVDexedStandaloneAccess::AVShowProductSettings = [this] { parmShow(); };
        Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
        AVDexedStandaloneAccess::AVOpenFiles = [safeThis](const StringArray& paths) {
            if (safeThis != nullptr) safeThis->AVOpenFilesFromSystem(paths);
        };
    }
    updateUI();
    startTimer(20);
    AVPresetStepPoll.startTimer(20);
    AVAdoptExternalSoundsOnce();
}

DexedAudioProcessorEditor::~DexedAudioProcessorEditor() {
    processor->setAVEditorOpen(false);
    if (JUCEApplicationBase::isStandaloneApp()) {
        AVDexedStandaloneAccess::AVShowProductSettings = {};
        AVDexedStandaloneAccess::AVOpenFiles = {};
    }
    PopupMenu::dismissAllActiveMenus();  // their LookAndFeel belongs to this editor
    removeMouseListener(&focusModality);
    Desktop::getInstance().removeFocusChangeListener(this);
    if (AVSoundKeysTarget != nullptr) AVSoundKeysTarget->removeKeyListener(&AVSoundKeys);
    stopTimer();
    AVPresetStepPoll.stopTimer();
    if (AVStepLearn >= 0) processor->cancelAVPresetStepLearn();
    processor->midiCCMapper->cancelMapping();
    if (AVSettingsOverlay) AVDexedComponentFade::forget(*AVSettingsOverlay);
    // Closed dialogs may still be fading out: their pictures live in the
    // frame and go with it.
    for (auto& prompt : AVPrompts)
        AVDexedComponentFade::forget(*prompt);
    AVPrompts.clear();
    AVDexedComponentFade::deleteAllIn(frameComponent);   // dialogs, Settings, Play's effect window
    AVDexedComponentFade::deleteAllIn(*this);            // an alert's veil
    // Waits for a running adoption (it only copies a few small files); one
    // still queued is dropped and the next editor tries again.
    AVAdoptionPool.reset();
    int queued = avAdoptionQueued;
    avAdoptionState.compare_exchange_strong(queued, avAdoptionIdle);
    midiKeyboard.releaseAVUINotes();
    AVPageFade.finish();
    for (auto* page : std::initializer_list<Component*> { &classicView, &playView, &layersView })
        Desktop::getInstance().getAnimator().cancelAnimation(page, false);   // a JUCE cross-fade (AVPageFade could not)
    AVDexedLanguageService::getInstance().removeChangeListener(this);
    setLookAndFeel(nullptr);
}

//==============================================================================
void DexedAudioProcessorEditor::AVApplyLowPower() {
    if (AVDexedTheme::lowPower == processor->lowPowerUI) return;
    AVDexedTheme::lowPower = processor->lowPowerUI;
    // The text in the background image changes tone; everything repaints.
    AVBackgroundLayer.invalidate();
    repaint();
}

void DexedAudioProcessorEditor::paint (Graphics& g) {
    // Wholly under the page shown, when that page is opaque (Play), or under
    // the tab change's cross-fade, none of the background shows: every glow
    // frame used to paint it for nothing.
    const auto covers = [this, &g](const Component& cover) {
        const auto area = getLocalArea(&cover, cover.getLocalBounds().toFloat());
        const int left = int(std::ceil(area.getX())), top = int(std::ceil(area.getY()));
        const Rectangle<int> inside(left, top, int(std::floor(area.getRight())) - left, int(std::floor(area.getBottom())) - top);
        return inside.contains(g.getClipBounds());
    };
    auto& page = AVViewComponent(AVVisibleView);
    if (page.isOpaque() && page.isVisible() && page.getAlpha() >= 1.0f && covers(page))
        return;
    if (AVPageFade.isVisible() && covers(AVPageFade))
        return;
    AVDrawBackground(g);
}

void DexedAudioProcessorEditor::AVDrawBackground(Graphics& g) {
    const std::array<int, 4> signature { processor->showKeyboard ? 1 : 0, frameComponent.getHeight(),
                                         playTabButton.getRight(), advancedTabButton.getRight() };
    if (signature != AVBackgroundSignature) {
        AVBackgroundSignature = signature;
        AVBackgroundLayer.invalidate();
    }
    AVBackgroundLayer.draw(g, *this, getLocalBounds().toFloat(), [this](Graphics& bg) { AVPaintBackground(bg); });
}

Rectangle<int> DexedAudioProcessorEditor::AVPageArea() const {
    return playView.getBounds().getUnion(classicView.getBounds()).getUnion(layersView.getBounds());
}

void DexedAudioProcessorEditor::AVPaintUnderPages(Graphics& g) {
    Graphics::ScopedSaveState state(g);
    g.addTransform(frameComponent.getTransform().inverted());
    AVDrawBackground(g);
}

void DexedAudioProcessorEditor::AVPaintBackground(Graphics& g) {
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
    const auto identity = AVGetIdentityLayout();
    AVDexedTheme::drawGreyText(g, "AV Vibes", avIdentityFont(), identity.text, Justification::centredLeft);
    AVDexedTheme::fillBox(g, identity.rule, 0.0f, false, 3.0f);
    // Separator between the tabs.
    for (const auto* tab : { &playTabButton, &advancedTabButton }) {
        const auto separator = Rectangle<float>(float(tab->getRight()) + 10.0f, AVTopBarAxis - 13.0f, 2.0f, 26.0f);
        AVDexedTheme::fillBox(g, separator, 0.0f, false, 4.0f);
    }
}

DexedAudioProcessorEditor::AVIdentityLayout DexedAudioProcessorEditor::AVGetIdentityLayout() {
    static const auto layout = [] {
        const auto font = avIdentityFont();
        AVIdentityLayout result;
        result.text = { 33.0f, AVTopBarAxis + 22.0f, 80.0f, 18.0f };
        // drawText centres the line box (ascent + descent) in its area, which
        // puts the baseline here.
        const float baseline = result.text.getCentreY() - font.getHeight() * 0.5f + font.getAscent();
        // The rule's centre is the middle of the x-height, measured on the
        // glyph "x" itself; Montserrat draws its own dashes there. The middle of
        // the line box, used before, sits about 1 px higher: next to "ibes"
        // (the letters beside the rule) it read as off-centre.
        GlyphArrangement xGlyph;
        xGlyph.addLineOfText(font, "x", 0.0f, 0.0f);   // baseline at y = 0
        Path ink;
        xGlyph.createPath(ink);
        const float centre = baseline + ink.getBounds().getCentreY();
        const float x = result.text.getX() + GlyphArrangement::getStringWidth(font, "AV Vibes") + 8.0f;
        // 2 px tall, on whole pixels so it stays crisp at 100 %.
        result.rule = { x, std::round(centre - 1.0f), 56.0f, 2.0f };
        return result;
    }();
    return layout;
}

Component& DexedAudioProcessorEditor::AVViewComponent(AVView view) {
    switch (view) {
        case AVView::play: return playView;
        case AVView::advanced: return classicView;
        case AVView::layers: break;
    }
    return layersView;
}

void DexedAudioProcessorEditor::showAVDexedPlayView(bool showPlay, bool animate) {
    showAVView(showPlay ? AVView::play : AVView::advanced, animate);
}

void DexedAudioProcessorEditor::showAVView(AVView view, bool animate) {
    midiKeyboard.releaseAVUINotes();
    if (cartManagerCover.isVisible()) cartManager.hideCartridgeManager();
    PopupMenu::dismissAllActiveMenus();
    const auto previous = AVVisibleView;
    auto& incoming = AVViewComponent(view);
    auto& outgoing = AVViewComponent(previous);
    const bool fade = animate && previous != view;
    // GOZO tab change: a 220 ms linear cross-fade (ComponentAnimator's fadeOut
    // of the old page with fadeIn of the new one). It is drawn from pictures
    // at the window's pixels on the screen's refresh (AVDexedCrossfade): each
    // frame mixes three pictures instead of painting a live page into a
    // transparency layer at 50 Hz. A fade still running is where the next starts.
    const auto area = AVPageArea().toFloat();
    const auto paintPage = [this](Graphics& g, Component& page) {
        AVPaintUnderPages(g);
        Graphics::ScopedSaveState state(g);
        g.addTransform(AffineTransform::translation(page.getPosition().toFloat()));
        page.paintEntireComponent(g, false);
    };
    bool pictured = false;
    if (fade)
        pictured = AVPageFade.takeCurrentFrameAsOutgoing(area)
                   || AVPageFade.outgoing.make(frameComponent, area, true, [&](Graphics& g) { paintPage(g, outgoing); });
    AVPageFade.finish();
    // No picture at the window's pixels (a window at an unusual scale): JUCE's
    // own cross-fade, as before.
    const bool legacy = fade && ! pictured;
    // The pages as they stay: only the new one shown (also under the fade,
    // so it takes the focus and clicks at once).
    for (auto other : { AVView::play, AVView::advanced, AVView::layers }) {
        auto& page = AVViewComponent(other);
        Desktop::getInstance().getAnimator().cancelAnimation(&page, false);
        page.setAlpha(1.0f);
        page.setInterceptsMouseClicks(true, true);
        if (other != view && ! (legacy && other == previous)) page.setVisible(false);
    }
    if (legacy) {
        Desktop::getInstance().getAnimator().fadeOut(&outgoing, 220);   // hides it; a proxy fades
        Desktop::getInstance().getAnimator().fadeIn(&incoming, 220);
    } else {
        incoming.setVisible(true);
    }
    AVVisibleView = view;
    playTabButton.setToggleState(view == AVView::play, dontSendNotification);
    advancedTabButton.setToggleState(view == AVView::advanced, dontSendNotification);
    layersTabButton.setToggleState(view == AVView::layers, dontSendNotification);
    if (view == AVView::play)
        playView.refreshLabels();
    if (pictured) {
        // What lies under the pages changes only with the layout: kept.
        if (AVPageFadeUnderSignature != AVBackgroundSignature || ! AVPageFade.under.matches(frameComponent, area)) {
            pictured = AVPageFade.under.make(frameComponent, area, true, [this](Graphics& g) { AVPaintUnderPages(g); });
            AVPageFadeUnderSignature = AVBackgroundSignature;
        }
        pictured = pictured && AVPageFade.incoming.make(frameComponent, area, true, [&](Graphics& g) { paintPage(g, incoming); });
    }
    // Without a window nobody sees a fade: the change is then immediate.
    if (pictured) {
        const bool outgoingOnTop = frameComponent.getIndexOfChildComponent(&outgoing)
                                   > frameComponent.getIndexOfChildComponent(&incoming);
        AVPageFade.setBounds(area.toNearestInt());
        AVPageFade.start(frameComponent, outgoingOnTop, 220.0, {});
    }
    if (view == AVView::advanced)
        AVMaybeShowAdvancedNotice();
}

void DexedAudioProcessorEditor::AVMaybeShowAdvancedNotice() {
    if (processor->advancedNoticeHidden || AVAdvancedNoticeShown || !isShowing())
        return;
    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    Timer::callAfterDelay(240, [safeThis] {
        if (safeThis == nullptr || safeThis->AVAdvancedNoticeShown || safeThis->AVVisibleView != AVView::advanced
            || !safeThis->isShowing() || !safeThis->AVCanShowNoticeNow())
            return;
        safeThis->AVAdvancedNoticeShown = true;
        auto& language = AVDexedLanguageService::getInstance();
        AVDexedPromptOverlay::Options options;
        options.title = language.text("advancedCpuTitle");
        options.message = language.text("advancedCpuMessage");
        options.confirmText = language.text("gotIt");
        options.altText = language.text("dontShowAgain");
        options.onAlt = [safeThis] {
            if (safeThis == nullptr) return;
            safeThis->processor->advancedNoticeHidden = true;
            safeThis->processor->savePreference();
        };
        safeThis->showAVPrompt(std::move(options), {});
    });
}

void DexedAudioProcessorEditor::applyLanguage() {
    auto& language = AVDexedLanguageService::getInstance();
    AVDropOverlay.setHint(language.text("dropBanksHere"));
    playTabButton.setButtonText(language.text("play"));
    advancedTabButton.setButtonText(language.text("advanced"));
    layersTabButton.setButtonText(language.text("layers"));
    layersTabButton.setTooltip(language.text("layersTooltip"));
    layersView.applyLanguage();
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
    AVDexedPromptOverlay::Options options;
    options.title = language.text("saveChangesTitle");
    options.message = language.text("saveChanges");
    options.confirmText = language.text("save");
    options.altText = language.text("discard");
    options.cancelText = language.text("cancel");
    // "Guardar" can rewrite a bank voice in its file: Return (pressed while
    // arrowing through sounds) must never choose it.
    options.returnConfirms = false;
    options.onAlt = [replacement] { replacement(); };
    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    showAVPrompt(std::move(options), [safeThis, replacement](bool confirmed, const String&) {
        if (safeThis == nullptr)
            return;
        if (!confirmed) {
            safeThis->updateUI();
            return;
        }
        // The same "Guardar" as the Save button; the new sound loads only once
        // the current one is safely saved.
        safeThis->AVSaveSound([safeThis, replacement](bool saved) {
            if (safeThis == nullptr)
                return;
            if (saved)
                replacement();
            else
                safeThis->updateUI();
        });
    });
}

void DexedAudioProcessorEditor::cartShow() {
    stopTimer();
    cartManager.resetActiveSysex();
    cartManager.refreshFiles();
    // A save while it was closed ("Guardar" in Play) may have changed the previewed bank.
    cartManager.reloadPreview();
    cartManagerCover.setBounds(classicView.getLocalBounds());
    cartManager.setBounds(16, 16, cartManagerCover.getWidth() - 32, cartManagerCover.getHeight() - 32);
    cartManager.updateCartFilename();
    cartManagerCover.setVisible(true);
    cartManager.initialFocus();
}

std::unique_ptr<ComponentTraverser> DexedAudioProcessorEditor::createFocusTraverser() {
    return std::make_unique<FocusTraverser>();
}

std::unique_ptr<ComponentTraverser> DexedAudioProcessorEditor::createKeyboardFocusTraverser() {
    // The frame holds the keyboard when no control does, but it is not a stop
    // of the Tab cycle: one with no ring, where nothing seems focused.
    struct SkipFrame final : public KeyboardFocusTraverser {
        explicit SkipFrame(Component& frameToSkip) : frame(frameToSkip) {}
        Component* getNextComponent(Component* current) override {
            auto* next = KeyboardFocusTraverser::getNextComponent(current);
            return next == &frame ? KeyboardFocusTraverser::getNextComponent(next) : next;
        }
        Component* getPreviousComponent(Component* current) override {
            auto* previous = KeyboardFocusTraverser::getPreviousComponent(current);
            return previous == &frame ? KeyboardFocusTraverser::getPreviousComponent(previous) : previous;
        }
        Component* getDefaultComponent(Component* parent) override {
            auto* first = KeyboardFocusTraverser::getDefaultComponent(parent);
            return first == &frame ? KeyboardFocusTraverser::getNextComponent(first) : first;
        }
        std::vector<Component*> getAllComponents(Component* parent) override {
            auto all = KeyboardFocusTraverser::getAllComponents(parent);
            all.erase(std::remove(all.begin(), all.end(), &frame), all.end());
            return all;
        }
        Component& frame;
    };
    return std::make_unique<SkipFrame>(frameComponent);
}

void DexedAudioProcessorEditor::loadCart(File file) {
    auto& language = AVDexedLanguageService::getInstance();
    Cartridge cart;

    int rc = cart.load(file);

    if ( rc < 0 ) {
        showAVNotice(language.text("presetError"), language.format("bankOpenError", {{"path", file.getFullPathName()}}));
        return;
    }

    if ( rc != 0 ) {
        // A file of single voices (no bank): its first voice becomes the sound
        // being played, as when one arrives by SysEx.
        MemoryBlock bytes;
        if (file.getSize() <= AVDexedLibrary::maxImportBytes && file.loadFileAsData(bytes)) {
            const auto contents = AVDexedSysex::scan(bytes);
            if (contents.banks.empty() && ! contents.voices.empty()) {
                std::array<uint8_t, AVDexedSysex::voiceBytes + 1> voice {};
                std::copy(contents.voices.front().begin(), contents.voices.front().end(), voice.begin());
                voice[AVDexedSysex::voiceBytes] = AVDexedSysex::checksum(voice.data(), AVDexedSysex::voiceBytes);
                processor->updateProgramFromSysex(voice.data());
                return;
            }
        }
        AVDexedPromptOverlay::Options options;
        options.title = language.text("invalidBankTitle");
        options.message = language.text("invalidBank") + " " + language.text("invalidBankQuestion");
        options.confirmText = language.text("loadRandom");
        options.cancelText = language.text("cancel");
        options.returnConfirms = false;   // random data only when chosen on purpose
        Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
        showAVPrompt(std::move(options), [safeThis, cart, file](bool confirmed, const String&) {
            if (safeThis != nullptr && confirmed)
                safeThis->applyCart(cart, file);
        });
        return;
    }

    applyCart(cart, file);
}

void DexedAudioProcessorEditor::importBanks(const Array<File>& dropped) {
    if (dropped.isEmpty()) return;
    // A DX7II or TX802 performance file (no banks in it) opens in the Layers page.
    Array<File> files;
    File performances;
    File layers;
    for (const auto& file : dropped) {
        if (file.isDirectory()) files.add(file);
        else if (avIsLayersPath(file.getFileName())) layers = file;
        else if (AVDexedLayersView::isPerformanceOnlyFile(file)) performances = file;
        else files.add(file);
    }
    if (performances != File() || layers != File())
        showAVView(AVView::layers);
    if (performances != File())
        layersView.loadPerformanceFile(performances);
    if (layers != File())
        layersView.loadLayersFile(layers);
    if (files.isEmpty()) return;
    auto& language = AVDexedLanguageService::getInstance();
    const auto root = AVDexedLibrary::rootFor(DexedAudioProcessor::dexedAppDir);
    const auto found = avCollectImportFiles(files);
    if (found.banks.isEmpty() && found.presets.isEmpty()) {
        // Neither banks nor sounds were found: the neutral "Importar".
        showAVNotice(language.text("importAction"), language.text("importNothing"));
        return;
    }
    // A file in a hidden folder of the root (.deleted, .migration-backup) is
    // not really in the library: it is copied like one from outside.
    Array<File> outside;
    for (const auto* list : { &found.banks, &found.presets })
        for (const auto& file : *list)
            if (! avIsInVisibleLibrary(file, root)) outside.add(file);
    if (outside.isEmpty()) {
        // Everything is in the library already: nothing to copy, nothing to ask.
        // The footer says so: a sound dragged from its own library folder just
        // loads, which otherwise looks as if the drop had been ignored.
        showAVFooterNotice(avAlreadyInLibraryNotice(found, root));
        AVOpenImportedSounds(found.banks, found.presets, {}, {}, {});
        return;
    }

    // As in GOZO: ask the folder first, then copy (an accidental cancel never
    // leaves a copied file without a home). Suggested: the dropped folder, else
    // the first bank; presets alone go to the folder being browsed or "User Presets".
    auto suggestion = found.folderName;
    for (const auto& bank : found.banks)
        if (suggestion.isEmpty() && ! avIsInVisibleLibrary(bank, root)) suggestion = bank.getFileNameWithoutExtension();
    if (suggestion.isEmpty()) {
        const auto selected = playView.getSelectedLibraryFolder();
        suggestion = selected.isDirectory() && selected.getParentDirectory() == root && ! AVDexedLibrary::isReadOnly(selected, root)
            ? selected.getFileName() : String(AVDexedLibrary::userPresetsFolderName);
    }
    const bool onlyBanks = std::all_of(outside.begin(), outside.end(), [](const File& file) { return avIsBankPath(file.getFileName()); });
    AVDexedPromptOverlay::Options options;
    // "Importar banco" only for banks; single sounds (alone or mixed with
    // banks) get the neutral "Importar".
    options.title = language.text(onlyBanks ? "importBank" : "importAction");
    if (outside.size() == 1)
        options.message = onlyBanks ? language.format("importFolderQuestion", {{"bank", outside.getFirst().getFileName()}})
                                    : language.format("importFolderQuestionOne", {{"name", outside.getFirst().getFileName()}});
    else
        options.message = language.format(onlyBanks ? "importFolderQuestionMany" : "importFolderQuestionFiles",
                                          {{"count", String(outside.size())}});
    options.hasEditor = true;
    options.editorText = AVDexedLibrary::sanitizeFolderName(suggestion);
    options.placeholder = language.text("folderName");
    options.maxLength = 48;
    options.confirmText = language.text("importAction");
    options.cancelText = language.text("cancel");
    options.validate = [root](const String& text) {
        auto& lang = AVDexedLanguageService::getInstance();
        const auto name = AVDexedLibrary::sanitizeFolderName(text);
        if (name.isEmpty()) return lang.text("folderNameInvalid");
        if (AVDexedLibrary::isReadOnly(AVDexedLibrary::folderNamed(root, name), root)) return lang.text("packFolderReadOnly");
        return String();
    };
    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    showAVPrompt(std::move(options), [safeThis, found, root](bool confirmed, const String& text) {
        if (safeThis == nullptr || ! confirmed) return;
        auto& lang = AVDexedLanguageService::getInstance();
        const auto folder = AVDexedLibrary::folderNamed(root, AVDexedLibrary::sanitizeFolderName(text));
        using Status = AVDexedLibrary::ImportResult::Status;
        Array<File> banks, presets;
        StringArray refused, failed;
        int added = 0, alreadyThere = 0;   // sounds copied; files the folder already held
        AVDexedLibrary::FolderIndex folderContents;   // the folder is read once for the whole batch
        const auto record = [&](const AVDexedLibrary::ImportResult& result, const String& name, bool isBank) {
            auto& list = isBank ? banks : presets;
            if (result.status == Status::notABank) refused.add(name);
            else if (result.status == Status::failed) failed.add(name);
            else {
                // Content the folder already held is reused (it still loads),
                // but it was not added: only real copies count. One file can
                // give several banks (a 64-voice dump, an archive).
                for (const auto& produced : result.files) list.addIfNotAlreadyThere(produced);
                if (result.files.isEmpty()) list.add(result.file);
                if (result.status == Status::copied) added += isBank ? result.sounds : 1;
                else ++alreadyThere;
            }
        };
        const auto take = [&](const File& file, bool isBank) {
            auto& list = isBank ? banks : presets;
            if (avIsInVisibleLibrary(file, root)) { list.add(file); return; }   // already in the library: stays where it is
            record(isBank ? AVDexedLibrary::importBank(file, folder, &folderContents)
                          : AVDexedLibrary::importPreset(file, folder, &folderContents), file.getFileName(), isBank);
        };
        // Single voices (one DX7 voice per file) of one folder become banks of
        // 32 named after that folder; a lone one becomes a bank of its own.
        std::map<String, Array<File>> singleVoices;
        Array<File> bankFiles;
        for (const auto& bank : found.banks) {
            if (! avIsInVisibleLibrary(bank, root) && AVDexedLibrary::isSingleVoiceFile(bank))
                singleVoices[bank.getParentDirectory().getFullPathName()].add(bank);
            else
                bankFiles.add(bank);
        }
        for (const auto& [directory, voices] : singleVoices) {
            if (voices.size() < 2) { bankFiles.add(voices.getFirst()); continue; }
            record(AVDexedLibrary::importVoices(voices, folder, File(directory).getFileName(), &folderContents),
                   File(directory).getFileName(), true);
        }
        for (const auto& bank : bankFiles) take(bank, true);
        for (const auto& preset : found.presets) take(preset, false);
        if (added > 0)
            safeThis->showAVFooterNotice(lang.format(added == 1 ? "importedNoticeOne" : "importedNotice",
                                                     {{"folder", folder.getFileName()}, {"count", String(added)}}));
        else if (alreadyThere > 0)
            safeThis->showAVFooterNotice(lang.format("alreadyInFolder", {{"folder", folder.getFileName()}}));
        safeThis->AVOpenImportedSounds(banks, presets, added > 0 || alreadyThere > 0 ? folder : File(), refused, failed);
    });
}

void DexedAudioProcessorEditor::AVOpenImportedSounds(const Array<File>& banks, const Array<File>& presets,
                                                     const File& folder, StringArray refused, StringArray failed) {
    auto& language = AVDexedLanguageService::getInstance();
    const auto root = AVDexedLibrary::rootFor(DexedAudioProcessor::dexedAppDir);
    // The sound that loads: preferably one that landed in `folder` (the one shown).
    const auto inFolder = [&folder](const File& file) { return folder == File() || file.getParentDirectory() == folder; };
    File firstBank;
    Cartridge firstCart;
    for (const auto& bank : banks) {
        Cartridge cart;
        if (! AVDexedLibrary::readBank(bank, cart))
            refused.addIfNotAlreadyThere(bank.getFileName());
        else if (firstBank == File() || (inFolder(bank) && ! inFolder(firstBank))) { firstBank = bank; firstCart = cart; }
    }
    File firstPreset;
    for (const auto& preset : presets)
        if (firstPreset == File() || (inFolder(preset) && ! inFolder(firstPreset))) firstPreset = preset;
    const auto first = firstBank != File() ? firstBank : firstPreset;
    const auto shown = folder != File() ? folder : first.getParentDirectory();
    if (first != File() && shown.isAChildOf(root))
        playView.showLibraryFolder(shown.getRelativePathFrom(root).replaceCharacter('\\', '/'));
    else
        playView.refreshLibrary();
    if (cartManagerCover.isVisible()) cartManager.refreshFiles();
    if (firstBank != File())
        applyCart(firstCart, firstBank);
    else if (firstPreset != File())
        loadNativePreset(firstPreset);
    // Told after the load (and its save-changes question), never on top of it.
    if (! refused.isEmpty() || ! failed.isEmpty()) {
        String message;
        if (! refused.isEmpty()) message << language.format("importNotBanks", {{"names", avNameList(refused)}});
        if (! failed.isEmpty()) {
            if (message.isNotEmpty()) message << "\n";
            message << language.format("importFailed", {{"names", avNameList(failed)}});
        }
        // Worded as banks only when no single sound (.dexedpreset) was involved.
        const auto isSound = [](const String& name) { return avIsPresetPath(name); };
        const bool soundsInvolved = ! presets.isEmpty() || std::any_of(refused.begin(), refused.end(), isSound)
                                    || std::any_of(failed.begin(), failed.end(), isSound);
        showAVNotice(language.text(soundsInvolved ? "importAction" : "importBank"), message);
    }
}

void DexedAudioProcessorEditor::applyCart(Cartridge cart, File file) {
    requestPresetReplacement([this, cart = std::move(cart), file]() mutable {
        commitCart(std::move(cart), file);
    });
}

void DexedAudioProcessorEditor::commitCart(Cartridge cart, const File& file) {
    // The bank and its file first: setCurrentProgram applies the per-sound
    // settings embedded in that file. `cart` was read before the save-changes
    // prompt: when "Guardar" there wrote into this very file, the file is newer.
    processor->setActiveBank(AVDexedLibrary::currentBankOf(file, cart), file);
    rebuildProgramCombobox();
    processor->setCurrentProgram(0);
    global.programs->setSelectedId(processor->getCurrentProgram()+1, dontSendNotification);
    processor->updateHostDisplay();

    playView.setActiveUserPreset({});
    playView.updateBank();
    refreshPresetHeader();
    // The bank manager's "Based on" label: every load path ends here, after any
    // prompt, and the editor timer that refreshes it is stopped while it is open.
    cartManager.updateCartFilename();
}

void DexedAudioProcessorEditor::saveCart() {
    fileChooser = std::make_unique<FileChooser>(AVDexedLanguageService::getInstance().text("exportBankTitle"),
                                                AVDexedLibrary::rootFor(DexedAudioProcessor::dexedAppDir), avBankSaveFilter, true);
    fileChooser->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                             [this](const FileChooser& fc) {
                                 if ( fc.getResults().isEmpty() )
                                     return;
                                 if (AVRefuseReadOnlyDestination(fc.getResult()))
                                     return;
                                 Cartridge snapshot(processor->currentCart);
                                 snapshot.packProgram(processor->activeProgram.getRawData(), processor->getCurrentProgram(),
                                                      processor->activeProgram.getName());
                                 if (! AVWriteBankFile(snapshot, fc.getResult())) {
                                     showAVNotice(AVDexedLanguageService::getInstance().text("saveError"), avBankWriteError(fc.getResult()));
                                     return;
                                 }
                                 cartManager.reloadPreview();
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
    AVDexedSettingsPanel& AVGetPanel() noexcept { return *AVPanel; }
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
    actions.close = [safeThis] { if (safeThis) safeThis->AVCloseSettings(false); };
    actions.applyInstrumentSettings = [safeThis](ParamDialog* dialog) {
        if (!safeThis) return;
        float scale = safeThis->processor->getZoomFactor();
        const bool opened = dialog->getDialogValues(safeThis->processor->controllers, safeThis->processor->sysexComm,
                                                    &safeThis->processor->showKeyboard, &scale,
                                                    &safeThis->processor->lowPowerUI);
        safeThis->processor->setZoomFactor(scale);
        safeThis->AVApplyLowPower();
        safeThis->processor->savePreference();
        safeThis->resetSize();
        if (!opened) {
            auto& language = AVDexedLanguageService::getInstance();
            safeThis->showAVNotice(language.text("midiInterface"), language.text("midiPortsError"));
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
    // Added hidden and faded in BEFORE enterModalState(), which makes it visible:
    // ComponentAnimator::fadeIn() does nothing to a component that is already
    // visible and opaque, which left only the fade-out working.
    frameComponent.addChildComponent(*AVSettingsOverlay);
    AVSettingsOverlay->setBounds(frameComponent.getLocalBounds());
    AVDexedComponentFade::fadeIn(*AVSettingsOverlay, 100);
    AVSettingsOverlay->enterModalState(true);
}

void DexedAudioProcessorEditor::showAVSettingsPage(AVDexedSettingsPanel::AVPage page) {
    parmShow();
    if (auto* overlay = dynamic_cast<AVDexedSettingsOverlay*>(AVSettingsOverlay.get()))
        overlay->AVGetPanel().AVShowPage(page);
}

AVDexedPromptOverlay* DexedAudioProcessorEditor::showAVPrompt(AVDexedPromptOverlay::Options options, AVDexedPromptOverlay::Result result) {
    midiKeyboard.releaseAVUINotes();
    AVDropOverlay.hide(true);
    AVReleaseFinishedPrompts();
    // A dialog opened from another one's result, or while another still fades
    // out, would otherwise photograph that dialog: it reuses the first backdrop.
    const bool promptFading = std::any_of(AVPrompts.begin(), AVPrompts.end(), [](const auto& prompt) { return prompt->isFinishing(); });
    if ((! AVInPromptResult && ! promptFading) || ! AVPromptBackdrop.isValid())
        AVPromptBackdrop = createComponentSnapshot(getLocalBounds()).rescaled(frameComponent.getWidth(), frameComponent.getHeight());
    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    if (options.altText.isNotEmpty()) {
        auto onAlt = std::move(options.onAlt);
        options.onAlt = [safeThis, onAlt] {
            if (safeThis != nullptr) safeThis->AVDeliverPromptCallback(onAlt);
        };
    }
    auto prompt = std::make_unique<AVDexedPromptOverlay>(frameComponent, AVPromptBackdrop, std::move(options),
        [safeThis, result](bool confirmed, const String& text) {
            if (safeThis != nullptr)
                safeThis->AVDeliverPromptCallback([&result, confirmed, &text] { if (result) result(confirmed, text); });
        });
    auto* raw = prompt.get();
    AVPrompts.push_back(std::move(prompt));
    raw->present();
    return raw;
}

void DexedAudioProcessorEditor::showAVMidiLearn(Component& control, RangedAudioParameter& parameter) {
    auto& language = AVDexedLanguageService::getInstance();
    const auto id = parameter.getParameterID();
    const auto name = control.getTitle().isNotEmpty() ? control.getTitle() : language.parameterName(id, parameter.getName(100));
    AVDexedPromptOverlay::Options options;
    options.title = language.text("learnMidiTitle");
    options.message = language.format("midiLearnMessage", {{"parameter", name}});
    // A single pill, Cancel: the dialog only waits for the controller.
    options.confirmText = language.text("cancel");
    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    // Every way out of the dialog ends here: Cancel, Escape and a click outside
    // stop this Learn. After a learned CC the handler waits for nothing.
    const Component::SafePointer<AVDexedPromptOverlay> prompt = showAVPrompt(std::move(options), [safeThis, id](bool, const String&) {
        if (safeThis == nullptr) return;
        auto& mapper = *safeThis->processor->midiCCMapper;
        if (mapper.getAVLearningParameterId() == id) mapper.cancelMapping();
    });
    processor->midiCCMapper->startMapping(&parameter, [safeThis, prompt, name] {
        if (safeThis == nullptr) return;
        if (prompt != nullptr) prompt->dismiss();
        const auto cc = safeThis->processor->midiCCMapper->getAVLastLearnedCC();
        safeThis->showAVFooterNotice(AVDexedLanguageService::getInstance().format("learnComplete",
                                                                                  {{"cc", String(cc)}, {"parameter", name}}));
    });
}

void DexedAudioProcessorEditor::AVLearnPresetStep(int step) {
    // One Learn at a time: a menu answered late (behind another dialog) must
    // not stack a second waiting dialog or take over the running Learn.
    if (! isPositiveAndBelow(step, 2) || hasAVPromptOpen() || isCurrentlyBlockedByAnotherModalComponent()) return;
    auto& language = AVDexedLanguageService::getInstance();
    AVDexedPromptOverlay::Options options;
    options.title = language.text("learnMidiTitle");
    options.message = language.format("presetStepLearnMessage", {{"step", avPresetStepName(step)}});
    options.confirmText = language.text("cancel");   // a single pill: the dialog only waits
    const int token = ++AVStepLearnToken;
    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    // Every way out of the dialog ends here: Cancel, Escape and a click outside
    // stop the Learn. Once the CC is learned, AVFollowPresetStepCCs has already
    // cleared AVStepLearn and closes the dialog with dismiss().
    AVStepLearnPrompt = showAVPrompt(std::move(options), [safeThis, token](bool, const String&) {
        if (safeThis == nullptr || safeThis->AVStepLearnToken != token || safeThis->AVStepLearn < 0) return;
        safeThis->AVStepLearn = -1;
        safeThis->processor->cancelAVPresetStepLearn();
    });
    AVStepLearn = step;
    processor->startAVPresetStepLearn(static_cast<DexedAudioProcessor::AVPresetStep>(step));
}

void DexedAudioProcessorEditor::AVFollowPresetStepCCs() {
    const int learned = processor->consumeAVPresetStepLearned();
    if (isPositiveAndBelow(learned, 2)) {
        if (learned == AVStepLearn) {
            AVStepLearn = -1;   // the dialog's result must not cancel what was just learned
            if (AVStepLearnPrompt != nullptr) AVStepLearnPrompt->dismiss();
        }
        const int cc = processor->getAVPresetStepCC(static_cast<DexedAudioProcessor::AVPresetStep>(learned));
        if (cc >= 0)
            showAVFooterNotice(AVDexedLanguageService::getInstance().format("presetStepLearned",
                                   {{"step", avPresetStepName(learned)}, {"cc", String(cc)}}));
    }
    // Each press of a learned control is one step through the list shown in
    // Play, as a click on the arrow beside the sound name is. Presses while a
    // dialog, a menu or Settings is open are dropped, not kept for later.
    AVPendingSoundSteps += processor->consumeAVPresetSteps();
    if (! AVCanStepSounds()) {
        AVPendingSoundSteps = 0;
        return;
    }
    // Right after the editor opens the list is still being read: a press then
    // waits for it instead of being lost.
    if (AVPendingSoundSteps == 0 || playView.isWaitingForFirstIndex())
        return;
    const int steps = std::exchange(AVPendingSoundSteps, 0);
    for (int i = 0; i < std::abs(steps) && AVCanStepSounds(); ++i)
        playView.navigatePreset(steps > 0 ? 1 : -1);
}

bool DexedAudioProcessorEditor::AVCanStepSounds() const {
    return ! hasAVPromptOpen() && ! isCurrentlyBlockedByAnotherModalComponent()
        && (AVSettingsOverlay == nullptr || ! AVSettingsOverlay->isVisible());
}

bool DexedAudioProcessorEditor::AVStepSoundFromKey(const KeyPress& key) {
    const auto mods = key.getModifiers();
    if (! AVIsSoundStepKey(key) || mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
        return false;
    // A text field keeps its arrows (the search field moves through the list
    // itself); the bank manager's grid and file list use theirs.
    if (dynamic_cast<TextEditor*>(Component::getCurrentlyFocusedComponent()) != nullptr
        || cartManagerCover.isShowing() || ! AVCanStepSounds())
        return false;
    playView.navigatePreset(key.isKeyCode(KeyPress::upKey) ? -1 : 1);
    return true;
}

void DexedAudioProcessorEditor::globalFocusChanged(Component* focusedComponent) {
    if (AVSoundKeysTarget.getComponent() == focusedComponent && focusedComponent != nullptr)
        return;
    if (AVSoundKeysTarget != nullptr)
        AVSoundKeysTarget->removeKeyListener(&AVSoundKeys);
    AVSoundKeysTarget = nullptr;
    // The controls that use Up / Down themselves: JUCE's Slider steps its value
    // and ComboBox its choice (the Play knobs are plain Sliders). Buttons let
    // the keys through; text fields and lists keep theirs.
    if (focusedComponent != nullptr && isParentOf(focusedComponent)
        && (dynamic_cast<Slider*>(focusedComponent) != nullptr || dynamic_cast<ComboBox*>(focusedComponent) != nullptr)) {
        focusedComponent->addKeyListener(&AVSoundKeys);
        AVSoundKeysTarget = focusedComponent;
    }
}

void DexedAudioProcessorEditor::AVDeliverPromptCallback(const std::function<void()>& callback) {
    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    AVInPromptResult = true;
    if (callback) callback();
    if (safeThis == nullptr) return;
    // A notice that waited for this prompt follows it on the same backdrop.
    AVPumpNotices();
    AVInPromptResult = false;
    AVReleaseFinishedPrompts();
}

void DexedAudioProcessorEditor::AVReleaseFinishedPrompts() {
    // A closed prompt fades out as a picture over the frame. The prompt is
    // kept until that fade ends: a dialog opened meanwhile reuses its
    // backdrop instead of photographing the picture.
    AVPrompts.erase(std::remove_if(AVPrompts.begin(), AVPrompts.end(), [](const auto& prompt) {
                        return prompt->isFinishing() && ! AVDexedComponentFade::isFading(*prompt);
                    }), AVPrompts.end());
    const bool fading = std::any_of(AVPrompts.begin(), AVPrompts.end(), [](const auto& prompt) { return prompt->isFinishing(); });
    if (! fading || AVPromptSweepPending)
        return;
    AVPromptSweepPending = true;
    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    Timer::callAfterDelay(150, [safeThis] {
        if (safeThis == nullptr) return;
        safeThis->AVPromptSweepPending = false;
        safeThis->AVReleaseFinishedPrompts();
    });
}

bool DexedAudioProcessorEditor::hasAVPromptOpen() const {
    return std::any_of(AVPrompts.begin(), AVPrompts.end(), [](const auto& prompt) { return ! prompt->isFinishing(); });
}

void DexedAudioProcessorEditor::showAVNotice(const String& title, const String& message) {
    AVNoticeQueue.push_back({ title, message });
    // Inside a prompt's result the caller may still open the next prompt (the
    // save-changes question after an import): the notice waits for it.
    if (! AVInPromptResult)
        AVPumpNotices();
}

bool DexedAudioProcessorEditor::AVCanShowNoticeNow() const {
    if (hasAVPromptOpen())
        return false;
    if (Component::getNumCurrentlyModalComponents() == 0)
        return true;
    // Settings is a full page, not a dialog: its own errors may show over it.
    return AVOnlySettingsIsModal();
}

bool DexedAudioProcessorEditor::AVOnlySettingsIsModal() const {
    return Component::getNumCurrentlyModalComponents() == 1 && AVSettingsOverlay != nullptr
        && Component::getCurrentlyModalComponent() == AVSettingsOverlay.get();
}

void DexedAudioProcessorEditor::AVCloseSettings(bool instantly) {
    if (! AVSettingsOverlay || ! AVSettingsOverlay->isVisible())
        return;
    // Hide now; destroy after the button callback (or the drop) has unwound.
    AVDexedComponentFade::cancel(*AVSettingsOverlay);
    if (AVSettingsOverlay->isCurrentlyModal())
        AVSettingsOverlay->exitModalState(0);
    if (instantly)
        AVSettingsOverlay->setVisible(false);
    else
        AVDexedComponentFade::fadeOut(*AVSettingsOverlay, 133);   // GOZO settings timing: in ~6 frames, out ~8 frames
    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    Timer::callAfterDelay(1, [safeThis] {
        if (safeThis && safeThis->AVSettingsOverlay && !safeThis->AVSettingsOverlay->isVisible())
            safeThis->AVSettingsOverlay.reset();
    });
}

void DexedAudioProcessorEditor::AVPumpNotices() {
    // An open prompt pumps the queue itself when it closes.
    if (AVNoticeQueue.empty() || hasAVPromptOpen())
        return;
    if (! AVCanShowNoticeNow()) {
        // Something else is modal (a menu, a file chooser, the Store dialog):
        // look again shortly. Prompts pump the queue themselves when they close.
        if (! AVNoticePollPending) {
            AVNoticePollPending = true;
            Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
            Timer::callAfterDelay(250, [safeThis] {
                if (safeThis == nullptr) return;
                safeThis->AVNoticePollPending = false;
                safeThis->AVPumpNotices();
            });
        }
        return;
    }
    const auto notice = AVNoticeQueue.front();
    AVNoticeQueue.erase(AVNoticeQueue.begin());
    AVDexedPromptOverlay::Options options;
    options.title = notice.title;
    options.message = notice.message;
    showAVPrompt(std::move(options), {});
}

void DexedAudioProcessorEditor::showAVFooterNotice(const String& text) {
    if (text.isEmpty())
        return;
    AVNoticeText = text;
    AVNoticeStart = Time::getMillisecondCounterHiRes();
    AVHelpFadeStart = 0.0;
    AVNoticeAnimation.start();
    AVUpdateHelpText();
}

void DexedAudioProcessorEditor::AVAdoptExternalSoundsOnce() {
    // Banks and presets 0.1.0 played from outside the data folder are copied in,
    // once per process and off the message thread (a slow or sleeping drive must
    // not freeze the editor). Unreachable ones stay pending for a later run.
    const auto appDirectory = DexedAudioProcessor::dexedAppDir;
    int idle = avAdoptionIdle;
    if (! appDirectory.isDirectory() || ! avAdoptionState.compare_exchange_strong(idle, avAdoptionQueued))
        return;
    const auto preferences = DexedAudioProcessor::getPreferencesFile();
    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    AVAdoptionPool = std::make_unique<ThreadPool>(1);
    AVAdoptionPool->addJob([appDirectory, preferences, safeThis] {
        avAdoptionState = avAdoptionStarted;
        // Copies here; the preferences are written on the message thread, where
        // every other preferences write of this process happens.
        auto adoption = AVDexedLibrary::copyExternalSounds(appDirectory, preferences);
        MessageManager::callAsync([safeThis, preferences, adoption = std::move(adoption)] {
            AVDexedLibrary::settleExternalSounds(preferences, adoption);
            const auto& copied = adoption.copied;
            if (safeThis == nullptr || copied.isEmpty())
                return;
            safeThis->playView.refreshLibrary();
            if (safeThis->cartManagerCover.isVisible()) safeThis->cartManager.refreshFiles();
            // The footer is one line: three names at most.
            safeThis->showAVFooterNotice(AVDexedLanguageService::getInstance().format("adoptedNotice",
                                                                                      {{"names", avNameList(copied, 3)}}));
        });
    });
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
    // Text and wheel positions only: out of sight they ask for no painting.
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

    // The meters, the operators' levels and Play's glow move on the animation
    // clock, in the screen's frames, from the moment something can be seen
    // (the meter's floor is -60 dBFS) until all of it is still again.
    if (!AVLiveAnimation.isRunning() && !AVLiveIsHidden() && (processor->hasAVOutputPeak(0.001f) || processor->peekVoiceStatus()))
        AVLiveAnimation.start();
}

bool DexedAudioProcessorEditor::AVLiveIsHidden() const {
    if (!isShowing() || (getPeer() != nullptr && getPeer()->isMinimised()))
        return true;
    for (int i = 0; i < Component::getNumCurrentlyModalComponents(); ++i)
        if (isParentOf(Component::getCurrentlyModalComponent(i)))
            return true;
    return false;
}

bool DexedAudioProcessorEditor::AVUpdateLive(double now) {
    // Out of sight it stops; the timer starts it again once it can be seen.
    if (AVLiveIsHidden())
        return false;
    // The output meter at 30 Hz, as a DAW's; the levels below at the frame rate.
    if (now - AVLastMeterUpdate >= 1000.0 / 30.0 - 2.0) {
        const auto peaks = processor->consumeAVOutputPeaks();
        AVOutputMeter.AVUpdate(peaks.first, peaks.second, AVLastMeterUpdate > 0.0 ? float((now - AVLastMeterUpdate) / 1000.0) : 0.02f);
        AVLastMeterUpdate = now;
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
        // A tail may still sound with no voice playing (effects): until the
        // meter reaches its floor.
        return AVOutputMeter.AVDisplayedGain() > 0.0f;
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
    return true;
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

void DexedAudioProcessorEditor::AVSaveSound(std::function<void(bool)> completion) {
    auto& language = AVDexedLanguageService::getInstance();
    const auto root = AVDexedLibrary::rootFor(DexedAudioProcessor::dexedAppDir);
    const auto activeFile = playView.getActiveUserPreset();

    // A sound file of the library (not of a read-only pack, nor one waiting
    // hidden in .deleted): overwritten in place, while it is still this sound.
    if (activeFile.existsAsFile() && AVDexedLibrary::isPresetFile(activeFile)
        && avIsInVisibleLibrary(activeFile, root) && ! AVDexedLibrary::isReadOnly(activeFile, root)) {
        // Renamed or deleted elsewhere (Finder, another instance, another
        // project) and a new sound saved under the same name: the file now holds
        // another id, and that sound is never overwritten. A sound without id
        // (from 0.1.0) has nothing to compare and saves in place as before.
        const auto expectedId = processor->getAVPresetId().trim();
        const auto holdsOtherSound = [&expectedId](const File& file) {
            return expectedId.isNotEmpty() && readNativePresetId(file).trim() != expectedId;
        };
        if (! holdsOtherSound(activeFile)) {
            const auto oldKey = playView.getPresetKeyForFile(activeFile);
            auto presetId = expectedId.isNotEmpty() ? expectedId : readNativePresetId(activeFile);
            if (presetId.isEmpty())
                presetId = Uuid().toString();
            const auto state = createNativePresetState(*processor, presetId);
            bool written = false, replacedMeanwhile = false;
            {
                // Under the library lock, as the browser's rename and delete: a
                // preset another AV Dexed just moved, removed or replaced is not
                // touched here (checked again now that nobody else can write).
                const AVDexedLibrary::LibraryScope lock(3000);
                if (! state.isEmpty() && lock.held && activeFile.existsAsFile()) {
                    replacedMeanwhile = holdsOtherSound(activeFile);
                    written = ! replacedMeanwhile && AVDexedLibrary::writeAtomically(activeFile, state);
                }
            }
            if (! replacedMeanwhile) {
                if (! written) {
                    showAVNotice(language.text("saveError"), language.format("writeError", {{"path", activeFile.getFullPathName()}}));
                    if (completion) completion(false);
                    return;
                }
                playView.migratePresetKey(oldKey, "user:" + presetId);
                processor->setAVPresetContext(processor->getAVPresetName(), presetId, activeFile);
                processor->captureAVPresetBaseline();
                playView.setActiveUserPreset(activeFile);
                processor->updateHostDisplay();
                showAVFooterNotice(language.format("savedSound", {{"name", activeFile.getFileNameWithoutExtension()}}));
                if (completion) completion(true);
                return;
            }
        }
        AVSaveSoundAs(std::move(completion),
                      language.format("presetReplacedElsewhere", {{"name", activeFile.getFileNameWithoutExtension()}}));
        return;
    }

    // A voice of a bank of the library: into its own slot of that .syx, with its
    // modern settings (the processor re-reads the file and patches only that slot).
    if (activeFile.getFullPathName().isEmpty() && processor->canSaveAVSoundToBank()) {
        const Cartridge previousBank = processor->currentCart;
        const auto bankFile = processor->activeFileCartridge;
        const auto outcome = processor->saveAVSoundToBank(processor->activeProgram.getName());
        if (outcome == DexedAudioProcessor::AVBankSaveResult::saved) {
            // Favourites and folder entries follow the voice into the new bank content.
            playView.carryAVBankMetadata(previousBank, processor->currentCart);
            processor->updateHostDisplay();
            playView.refreshLibrary();
            // The bank manager's preview may show this file as it was before.
            cartManager.reloadPreview();
            updateUI();
            showAVFooterNotice(language.format("savedIn", {{"place", bankFile.getFileNameWithoutExtension()}}));
            if (completion) completion(true);
            return;
        }
        if (outcome == DexedAudioProcessor::AVBankSaveResult::failed) {
            showAVNotice(language.text("saveError"), language.format("writeError", {{"path", bankFile.getFullPathName()}}));
            if (completion) completion(false);
            return;
        }
        // needsSaveAs: the file no longer holds this bank (edited elsewhere).
    }

    // Factory or pack sounds, sounds from outside the library, SysEx banks…
    AVSaveSoundAs(std::move(completion));
}

void DexedAudioProcessorEditor::AVSaveSoundAs(std::function<void(bool)> completion, const String& reason) {
    auto& language = AVDexedLanguageService::getInstance();
    const auto root = AVDexedLibrary::rootFor(DexedAudioProcessor::dexedAppDir);
    // The folder being browsed when it is a real, writable one; else "User Presets".
    auto folder = playView.getSelectedLibraryFolder();
    if (! (folder.isDirectory() && avIsInVisibleLibrary(folder, root) && folder.hasWriteAccess()
           && ! AVDexedLibrary::isReadOnly(folder, root)))
        // Never a pack that happens to be called "User Presets" ("User Presets 2" then).
        folder = AVDexedLibrary::writableFolderFor(root, AVDexedLibrary::userPresetsFolderName);
    const auto place = avFolderDisplayName(folder, root);

    AVDexedPromptOverlay::Options options;
    options.title = language.text("saveAs");
    options.message = (reason.isNotEmpty() ? reason + "\n" : String()) + language.format("saveAsWhere", {{"folder", place}});
    options.hasEditor = true;
    options.editorText = processor->getAVPresetName().trim();
    options.placeholder = language.text("soundName");
    options.maxLength = 64;
    options.confirmText = language.text("save");
    options.cancelText = language.text("cancel");
    options.validate = [](const String& text) {
        return avPresetFileStem(text).isEmpty() ? AVDexedLanguageService::getInstance().text("soundNameInvalid") : String();
    };
    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    showAVPrompt(std::move(options), [safeThis, folder, place, completion = std::move(completion)](bool confirmed, const String& text) {
        if (safeThis == nullptr)
            return;
        if (!confirmed) {
            if (completion) completion(false);
            return;
        }
        auto& lang = AVDexedLanguageService::getInstance();
        auto* editor = safeThis.getComponent();
        const auto presetId = Uuid().toString();
        File destination;
        bool written = false;
        // The free name is taken and written under the library lock, so another
        // AV Dexed saving the same name at the same moment cannot overwrite it.
        const AVDexedLibrary::LibraryScope lock(4000);
        if (lock.held && folder.createDirectory().wasOk()) {
            destination = AVDexedLibrary::uniqueDestination(folder, avPresetFileStem(text) + AVDexedLibrary::presetExtension);
            const auto state = createNativePresetState(*editor->processor, presetId, destination.getFileNameWithoutExtension());
            written = ! state.isEmpty() && AVDexedLibrary::writeAtomically(destination, state);
        }
        if (! written) {
            editor->showAVNotice(lang.text("saveError"),
                                 lang.format("writeError", {{"path", (destination != File() ? destination : folder).getFullPathName()}}));
            if (completion) completion(false);
            return;
        }
        editor->processor->setAVPresetContext(destination.getFileNameWithoutExtension(), presetId, destination);
        editor->processor->captureAVPresetBaseline();
        editor->playView.setActiveUserPreset(destination);
        editor->processor->updateHostDisplay();
        editor->playView.refreshLibrary();
        editor->updateUI();
        editor->showAVFooterNotice(lang.format("savedIn", {{"place", place}}));
        if (completion) completion(true);
    });
}

bool DexedAudioProcessorEditor::AVWriteBankFile(Cartridge bank, const File& file, int storedSlot,
                                                const std::map<String, float>& storedSettings) {
    if (! bank.containsData() || file.getFullPathName().isEmpty())
        return false;
    // Another AV Dexed saving a sound into this bank re-reads it, checks it and
    // writes it back under the lock: a write in between would be lost.
    const AVDexedLibrary::LibraryScope lock(3000);
    if (! lock.held)
        return false;
    MemoryBlock original;
    AVDexedLibrary::BankSettings settings;
    if (file.existsAsFile()) {
        // A bank file is about 4 KB. One of 64 KB or more is an archive of many
        // banks: only its first 64 KB can be read, and writing them back would
        // cut every bank after that point.
        if (file.getSize() >= 65535 || ! file.loadFileAsData(original))
            return false;
        Cartridge onDisk;
        const int loadResult = onDisk.load(static_cast<const uint8_t*>(original.getData()), static_cast<int>(original.getSize()));
        if (loadResult == 0 || loadResult == 1)
            settings = AVDexedLibrary::readEmbeddedSettings(original);   // kept, with its other SysEx messages
        else if (original.getSize() <= SYSEX_SIZE)
            original.reset();   // too small to hold more than a bank: replaced, as Dexed always did
        else
            return false;       // other data that holds no bank is never rewritten
    }
    if (isPositiveAndBelow(storedSlot, 32))
        settings = avSettingsWithSlot(std::move(settings), bank, storedSlot, storedSettings);
    return AVDexedLibrary::writeAtomically(file, AVDexedLibrary::composeBankFile(original, bank, settings));
}

bool DexedAudioProcessorEditor::AVRefuseReadOnlyDestination(const File& destination) {
    const auto root = AVDexedLibrary::rootFor(DexedAudioProcessor::dexedAppDir);
    if (! destination.isAChildOf(root) || ! AVDexedLibrary::isReadOnly(destination, root))
        return false;
    auto& language = AVDexedLanguageService::getInstance();
    showAVNotice(language.text("saveError"), language.text("packFolderReadOnly"));
    return true;
}

void DexedAudioProcessorEditor::loadNativePreset(const File& file) {
    auto& language = AVDexedLanguageService::getInstance();
    if (!file.existsAsFile() || file.getSize() <= 0 || file.getSize() > 32 * 1024 * 1024) {
        showAVNotice(language.text("presetError"), language.text("missingPreset"));
        return;
    }
    MemoryBlock state;
    if (!file.loadFileAsData(state)) {
        showAVNotice(language.text("presetError"), language.format("readFileError", {{"path", file.getFullPathName()}}));
        return;
    }
    auto xml = AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    if (xml == nullptr) {
        showAVNotice(language.text("presetError"), language.text("invalidPreset"));
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
        showAVNotice(language.text("presetError"), language.text("invalidPreset"));
        return;
    }

    Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
    requestPresetReplacement([safeThis, file, state = std::move(state)]() mutable {
        if (safeThis == nullptr)
            return;
        if (!safeThis->processor->loadAVNativePreset(state.getData(), static_cast<int>(state.getSize()))) {
            auto& language = AVDexedLanguageService::getInstance();
            safeThis->showAVNotice(language.text("presetError"), language.text("invalidPreset"));
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
                                                    AVDexedLibrary::rootFor(DexedAudioProcessor::dexedAppDir), "*.syx;*.SYX;*.*", true);
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
                                     showAVNotice(AVDexedLanguageService::getInstance().text("presetError"),
                                                  AVDexedLanguageService::getInstance().text("readError"));
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
        // The bank as it was when the dialog opened, to notice a file that
        // changed meanwhile (another AV Dexed, Finder, an older DAW session).
        const auto baseFingerprint = AVDexedLibrary::bankFingerprint(destSysex);

        if (!destSysex.packProgram(storedVoice.getRawData(), programNum, programName)) {
            const auto& language = AVDexedLanguageService::getInstance();
            showAVNotice(language.text("saveError"), language.text("voicePackError"));
            cartManager.resetActiveSysex();
            return;
        }

        // The sound's settings now (volume, pan, reverb…), taken with the
        // program the way "Guardar" takes them.
        const auto liveSettings = [this] {
            const ScopedLock audioLock(processor->getCallbackLock());
            return processor->captureAVSoundSettings().values;
        };

        // `settingsKept`: the stored slot will play with the sound's settings
        // (they went into its file, or the file already gives them to that
        // voice). Only then is the sound taken as saved; otherwise it stays
        // "Modified", so "Guardar" and the save-changes prompt still protect them.
        auto adoptStoredBank = [this, programNum, programName](const Cartridge& savedCart, const File& source, bool settingsKept) {
            playView.carryAVBankMetadata(processor->currentCart, savedCart);
            processor->setActiveBank(savedCart, source);
            processor->activeProgram.setName(programName);
            processor->setAVPresetContext({}, {}, {});
            if (settingsKept)
                processor->captureAVPresetBaseline();
            processor->setCurrentProgramSelection(programNum);
            processor->updateHostDisplay();
            rebuildProgramCombobox();
            cartManager.resetActiveSysex();
            cartManager.reloadPreview();
        };

        // Into a file, the stored slot carries the sound's settings with it, as
        // with "Guardar"; the other slots keep theirs.
        auto writeStoredBank = [destSysex, programNum, liveSettings](const File& file) {
            return AVWriteBankFile(destSysex, file, programNum, liveSettings());
        };

        // Writes the whole bank over `file`. When the file no longer holds the
        // bank this dialog started from, its other 31 sounds would silently go
        // back to this copy: that is asked first (Return never confirms it).
        auto overwriteBank = [this, destSysex, baseFingerprint, adoptStoredBank, writeStoredBank](const File& file, bool adopt) {
            const auto write = [this, destSysex, file, adopt, adoptStoredBank, writeStoredBank] {
                if (! writeStoredBank(file)) {
                    showAVNotice(AVDexedLanguageService::getInstance().text("saveError"), avBankWriteError(file));
                    cartManager.resetActiveSysex();
                    return;
                }
                if (adopt) {
                    adoptStoredBank(destSysex, file, true);
                } else {
                    cartManager.resetActiveSysex();
                    cartManager.reloadPreview();
                }
            };
            // Only a file that does not exist is written without a look: an
            // existing one that is not a single readable bank (an archive of
            // several, 64 KB or more) is never rewritten.
            if (! file.existsAsFile()) {
                write();
                return;
            }
            Cartridge onDisk;
            if (! AVDexedLibrary::readBank(file, onDisk)) {
                const auto& language = AVDexedLanguageService::getInstance();
                showAVNotice(language.text("saveError"), language.format("bankNotRewritable", {{"bank", file.getFileName()}}));
                cartManager.resetActiveSysex();
                return;
            }
            if (AVDexedLibrary::bankFingerprint(onDisk) == baseFingerprint) {
                write();
                return;
            }
            auto& language = AVDexedLanguageService::getInstance();
            AVDexedPromptOverlay::Options options;
            options.title = language.text("storeProgramTitle");
            options.message = language.format("storeBankChanged", {{"bank", file.getFileName()}});
            options.confirmText = language.text("overwrite");
            options.cancelText = language.text("cancel");
            options.returnConfirms = false;
            Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
            showAVPrompt(std::move(options), [safeThis, write](bool confirmed, const String&) {
                if (safeThis == nullptr) return;
                if (confirmed) write();
                else safeThis->cartManager.resetActiveSysex();
            });
        };

        if ( ! hasExternalFile ) {
            int action = dialog->getComboBoxComponent("SaveAction")->getSelectedItemIndex();
            if (action == 0) {
                // Only in this session: no file is written, so the slot keeps the
                // sound's settings only if its file already gives them to that voice.
                const auto source = processor->activeFileCartridge;
                const auto heard = avSettingsHeard(AVDexedLibrary::readEmbeddedSettings(source),
                                                   AVDexedLibrary::voiceHash(destSysex, programNum), programNum);
                adoptStoredBank(destSysex, source, avSameSettings(liveSettings(), heard));
                return;
            }

            if (action == 1) {
                fileChooser = std::make_unique<FileChooser>(AVDexedLanguageService::getInstance().text("destinationSysex"),
                                                            AVDexedLibrary::rootFor(DexedAudioProcessor::dexedAppDir), avBankSaveFilter, true);
                fileChooser->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                                         [this, adoptStoredBank, writeStoredBank, destSysex](const FileChooser& fc) {
                                             if (fc.getResults().isEmpty()) {
                                                 cartManager.resetActiveSysex();
                                                 return;
                                             }

                                             File destination = fc.getResult();
                                             if (AVRefuseReadOnlyDestination(destination)) {
                                                 cartManager.resetActiveSysex();
                                                 return;
                                             }
                                             if (! writeStoredBank(destination)) {
                                                 showAVNotice(AVDexedLanguageService::getInstance().text("saveError"),
                                                              avBankWriteError(destination));
                                                 cartManager.resetActiveSysex();
                                                 return;
                                             }
                                             adoptStoredBank(destSysex, destination, true);
                                         });
                return;
            }

            File destination = processor->activeFileCartridge;
            if (AVRefuseReadOnlyDestination(destination)) {
                cartManager.resetActiveSysex();
                return;
            }
            if (!destination.existsAsFile()) {
                const auto& language = AVDexedLanguageService::getInstance();
                showAVNotice(language.text("saveError"),
                             language.format("writeError", {{"path", destination.getFullPathName()}}));
                cartManager.resetActiveSysex();
                return;
            }
            overwriteBank(destination, true);
            return;
        } else {
            if (AVRefuseReadOnlyDestination(externalFile)) {
                cartManager.resetActiveSysex();
                return;
            }
            overwriteBank(externalFile, externalFile == processor->activeFileCartridge);
            return;
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
    // Not while a dialog is open: the drop would be swallowed or close it.
    // Settings is a page, not a dialog: it gives way to the drop.
    if (hasAVPromptOpen() || (isCurrentlyBlockedByAnotherModalComponent() && ! AVOnlySettingsIsModal())) return false;
    // Any mix of banks, presets and folders is added to the library; a tuning
    // file loads on its own.
    if (avAllImportable(files)) return true;
    return files.size() == 1 && avIsTuningPath(files[0]);
}

void DexedAudioProcessorEditor::fileDragEnter(const StringArray& files, int, int) {
    auto& language = AVDexedLanguageService::getInstance();
    // Worded by what is dragged: single sounds, banks (or folders of them), a tuning.
    AVDropOverlay.setHint(language.text(avAllSounds(files) ? "dropSoundsHere"
                                        : avAllImportable(files) ? "dropBanksHere" : "dropTuningHere"));
    AVDropOverlay.setBounds(frameComponent.getLocalBounds());
    AVDropOverlay.show();
}

void DexedAudioProcessorEditor::fileDragExit(const StringArray&) {
    AVDropOverlay.hide(false);
}

void DexedAudioProcessorEditor::filesDropped (const StringArray &files, int, int) {
    // JUCE does not call fileDragExit after a drop.
    AVDropOverlay.hide(false);
    // Settings (JUCE already took it out of its modal state for the drop) goes
    // away at once: the import's question photographs the editor behind it.
    AVCloseSettings(true);
    // A drop from the Finder leaves the Finder in front: the folder question
    // would open behind it, with Return and the typed name going to the Finder.
    // Come to the front (in a host, the host comes).
    if (auto* peer = getPeer()) {
        Process::makeForegroundProcess();
        peer->toFront(true);
    }
    if (avAllImportable(files)) {
        Array<File> items;
        for (const auto& path : files) items.add(File(path));
        importBanks(items);
        return;
    }
    if (files.size() != 1 || !avIsTuningPath(files[0]))
        return;
    const auto& fn = files[0];
    const bool scale = fn.endsWithIgnoreCase(".scl");
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
        showAVNotice(language.text("tuningFileError"), problem);
        return;
    }
    if (scale)
        processor->applySCLTuning(file);
    else
        processor->applyKBMMapping(file);
}

void DexedAudioProcessorEditor::AVOpenFilesFromSystem(const StringArray& paths) {
    // What a drop on the window takes: banks, sounds and folders of them. A
    // tuning file is not a document of the app; anything else is left alone.
    for (const auto& path : paths)
        if (File::isAbsolutePath(path) && File(path).exists()
            && (avIsBankPath(path) || avIsPresetPath(path) || File(path).isDirectory()))
            AVPendingSystemFiles.addIfNotAlreadyThere(path);
    AVOpenPendingSystemFiles();
}

void DexedAudioProcessorEditor::AVOpenPendingSystemFiles() {
    if (AVPendingSystemFiles.isEmpty())
        return;
    // Settings is a page, not a dialog: it gives way to the import's question.
    if (AVOnlySettingsIsModal())
        AVCloseSettings(true);
    // A dialog must finish first, never stacked on: one that is open, one whose
    // result has not run yet (it arrives asynchronously and may open the next
    // dialog, "save changes" after an import), or any other modal thing (a
    // menu, a file chooser). Look again shortly, with a single poll however
    // many files arrive; the files wait as long as it takes.
    if (! AVPrompts.empty() || AVInPromptResult || isCurrentlyBlockedByAnotherModalComponent()) {
        if (AVPendingSystemPollPending)
            return;
        AVPendingSystemPollPending = true;
        Component::SafePointer<DexedAudioProcessorEditor> safeThis(this);
        Timer::callAfterDelay(300, [safeThis] {
            if (safeThis == nullptr) return;
            safeThis->AVPendingSystemPollPending = false;
            safeThis->AVOpenPendingSystemFiles();
        });
        return;
    }
    Array<File> items;
    for (const auto& path : AVPendingSystemFiles) items.add(File(path));
    AVPendingSystemFiles.clear();
    importBanks(items);
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
    // A dialog's keys bubble up to here: no shortcut acts behind it (Ctrl+P
    // would stack Settings on a modal dialog, Ctrl+L open the bank manager).
    if (hasAVPromptOpen() || isCurrentlyBlockedByAnotherModalComponent())
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
        // The bank manager lives in Advanced: from Play, go there first, so it
        // is never left open behind a hidden view.
        if (playView.isVisible()) showAVDexedPlayView(false, false);
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

    // Up / Down that no control used: the previous / next sound.
    return AVStepSoundFromKey(key);
}

void DexedAudioProcessorEditor::AVUpdateFooter() {
    auto& language = AVDexedLanguageService::getInstance();
    AVUpdateHelpText();
    // CPU and voices read four times a second, like a status bar, not per frame.
    if (++AVStatusTicks >= 12) {
        AVStatusTicks = 0;
        AVCpuLabel.setText(String::fromUTF8("CPU · ") + String(processor->getAVAudioCpuPercent(), 1) + "%", dontSendNotification);
        int layers = 1;
        if (processor->isAVLayerModeOn()) {
            layers = 0;
            for (int layer = 0; layer < DexedAudioProcessor::AVLayerCount; ++layer)
                layers += processor->getAVLayerSettings(layer).enabled.load() ? 1 : 0;
        }
        AVVoicesLabel.setText(language.text("voices") + ": " + String(processor->getAVActiveVoices()) + " / " + String(16 * jmax(1, layers)), dontSendNotification);
    }
}

void DexedAudioProcessorEditor::AVUpdateHelpText() {
    const auto now = Time::getMillisecondCounterHiRes();
    // A footer notice wins over the hover help while it lasts: in, held, out.
    if (AVNoticeText.isNotEmpty()) {
        const auto elapsed = now - AVNoticeStart;
        if (elapsed < noticeHoldUntil + noticeFadeOut) {
            const auto alpha = elapsed < noticeFadeIn ? elapsed / noticeFadeIn
                             : elapsed > noticeHoldUntil ? 1.0 - (elapsed - noticeHoldUntil) / noticeFadeOut : 1.0;
            AVHelpLabel.setColour(Label::textColourId, AVDexedTheme::textPrimary);
            AVHelpLabel.setText(AVNoticeText, dontSendNotification);
            AVDexedDim::setAlpha(AVHelpLabel, float(jlimit(0.0, 1.0, alpha)));
            return;
        }
        AVNoticeText.clear();
        AVHelpFadeStart = now;   // the normal help fades back in
        AVHelpLabel.setColour(Label::textColourId, AVDexedTheme::textSecondary);
    }
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
    AVHelpLabel.setText(help.isEmpty() ? AVDexedLanguageService::getInstance().text("footerHelp") : help, dontSendNotification);
    float alpha = 1.0f;
    if (AVHelpFadeStart > 0.0) {
        const auto elapsed = now - AVHelpFadeStart;
        if (elapsed < helpFadeIn) alpha = float(elapsed / helpFadeIn);
        else AVHelpFadeStart = 0.0;
    }
    AVDexedDim::setAlpha(AVHelpLabel, alpha);
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
    AVPageFade.finish();   // its pictures are of the old layout
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
    const int layersWidth = roundToInt(GlyphArrangement::getStringWidth(tabFont, layersTabButton.getButtonText())) + 12;
    // Everything in the top bar shares one centre line (AVTopBarAxis): the logo
    // mark, the tab text, the preset name with its arrows and star, and the
    // icons, which keep GOZO's even 14 px gaps against a 30 px right margin.
    const int axis = int(AVTopBarAxis);
    playTabButton.setBounds(262, axis - 21, playWidth, 42);
    advancedTabButton.setBounds(playTabButton.getRight() + 22, axis - 21, advancedWidth, 42);
    layersTabButton.setBounds(advancedTabButton.getRight() + 22, axis - 21, layersWidth, 42);
    AVSettingsButton.setBounds(WINDOW_SIZE_X - 60, axis - 15, 30, 30);
    AVOutputMeter.setBounds(AVSettingsButton.getX() - 14 - 16, axis - 15, 16, 30);
    saveButton.setBounds(AVOutputMeter.getX() - 14 - 30, axis - 15, 30, 30);
    const int navLeft = layersTabButton.getRight() + 50;
    playView.getPresetNavigator().setBounds(navLeft, axis - 21, saveButton.getX() - 50 - navLeft, 58);
    const int bottom = processor->showKeyboard ? 158 : 30;
    const int contentBottom = height - bottom;
    playView.setBounds(0, topBar, WINDOW_SIZE_X, contentBottom - topBar);
    const auto content = Rectangle<int>(20, topBar + 2, WINDOW_SIZE_X - 40, contentBottom - topBar - 10);
    classicView.setBounds(content);
    layersView.setBounds(content);
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
    AVDropOverlay.setBounds(frameComponent.getLocalBounds());
    if (cartManagerCover.isVisible()) {
        cartManagerCover.setBounds(classicView.getLocalBounds());
        cartManager.setBounds(cartManagerCover.getLocalBounds().reduced(16));
    }
}

void DexedAudioProcessorEditor::resetSize() {
    const float factor = jlimit(AVMinZoom, 1.6f, processor->getZoomFactor());
    processor->setZoomFactor(factor);
    const int logicalHeight = processor->showKeyboard ? WINDOW_SIZE_Y : WINDOW_SIZE_Y - 112;
    frameComponent.setBounds(0, 0, WINDOW_SIZE_X, logicalHeight);
    frameComponent.setTransform(AffineTransform::scale(factor));
    AVLayoutFrame();
    setResizeLimits(roundToInt(WINDOW_SIZE_X * AVMinZoom), roundToInt(logicalHeight * AVMinZoom),
                    roundToInt(WINDOW_SIZE_X * 1.6f), roundToInt(logicalHeight * 1.6f));
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio(double(WINDOW_SIZE_X) / logicalHeight);
    const int width = roundToInt(WINDOW_SIZE_X * factor);
    setSize(width, roundToInt(logicalHeight * (float(width) / WINDOW_SIZE_X)));
}

void DexedAudioProcessorEditor::resized() {
    if (processor == nullptr || getWidth() <= 0) return;
    const float factor = jlimit(AVMinZoom, 1.6f, float(getWidth()) / WINDOW_SIZE_X);
    frameComponent.setTransform(AffineTransform::scale(factor));
    if (!approximatelyEqual(factor, processor->getZoomFactor())) {
        processor->setZoomFactor(factor);
        resizeSaveCountdown = 500;
    }
    repaint();
}
