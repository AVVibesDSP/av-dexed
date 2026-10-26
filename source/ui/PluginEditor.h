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
 */

#ifndef PLUGINEDITOR_H_INCLUDED
#define PLUGINEDITOR_H_INCLUDED

#include "../JuceLibraryCode/JuceHeader.h"
#include "PluginProcessor.h"
#include <set>
#include "OperatorEditor.h"
#include "GlobalEditor.h"
#include "component/DXComponents.h"
#include "DXLookNFeel.h"
#include "window/CartManager.h"
#include "component/ParameterObserver.h"
#include "AVDexedPlayView.h"
#include "AVDexedLayersView.h"
#include "AVDexedKeyboard.h"
#include "AVDexedResizeGrip.h"
#include "AVDexedWheel.h"
#include "AVDexedPeakMeter.h"
#include "AVDexedPromptOverlay.h"
#include "AVDexedPixelCache.h"
#include "AVDexedAnimationClock.h"
#include "AVDexedDropOverlay.h"
#include "AVDexedFade.h"
#include "window/AVDexedSettingsPanel.h"

class AVDexedPanicButton final : public juce::TextButton {
public:
    AVDexedPanicButton();
    void setLabel(const juce::String& text);
    void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;

private:
    std::unique_ptr<juce::Drawable> icon;
    juce::String label;
};

//==============================================================================
/**
*/
class DexedAudioProcessorEditor  : public AudioProcessorEditor, public ComboBox::Listener, public Timer,
                                   public FileDragAndDropTarget, public KeyListener, public AVDexedAnimationHost,
                                   private ChangeListener, private FocusChangeListener {
    // Declared first so all child components/dialogs release their LAF before it dies.
    SharedResourcePointer<DXLookNFeel> lookAndFeel;
    /** Every animation of the editor runs on this clock, in the screen's frames. */
    AVDexedAnimationClock AVClock { *this };
    /** The output meter (30 Hz), the operators' meters and envelope steps, and
        Play's glow targets: from the first audible sound until all is still. */
    bool AVUpdateLive(double now);
    /** Nothing the live updates draw can be seen: the window is minimised or
        hidden, or an in-editor page covers the editor (Settings, an effect's
        panel, a dialog: all modal, all over a blurred copy of the editor). */
    bool AVLiveIsHidden() const;
    AVDexedAnimation AVLiveAnimation { *this, [this](double now) { return AVUpdateLive(now); } };
    /** Up / Down choose the previous / next sound anywhere in the editor. A
        focused knob or combo box would take them first (JUCE gives a key to the
        focused control before its parents), so this listener rides on the
        focused control (globalFocusChanged) and hands them to the editor. It
        outlives every control it is attached to. */
    struct AVSoundStepKeys final : public KeyListener {
        explicit AVSoundStepKeys(DexedAudioProcessorEditor& owner) : editor(owner) {}
        bool keyPressed(const KeyPress& key, Component*) override { return editor.AVStepSoundFromKey(key); }
        DexedAudioProcessorEditor& editor;
    } AVSoundKeys { *this };
    Component::SafePointer<Component> AVSoundKeysTarget;
    void globalFocusChanged(Component* focusedComponent) override;
    /** Up / Down (no Cmd, Ctrl or Alt): the previous / next sound of the Play
        list, unless a text field has the focus or the bank manager is open, or
        sounds cannot change now (AVCanStepSounds). */
    bool AVStepSoundFromKey(const KeyPress& key);
    /** Sounds may change from the keys or a learned MIDI control now: no
        dialog, menu or other modal window, and Settings closed. */
    bool AVCanStepSounds() const;
    /** The keyboard's home: a click on empty space gives it the focus (not the
        first control, which was the Play tab), so keys still reach the editor. */
    Component frameComponent;
    Component classicView;
    AVDexedKeyboardComponent midiKeyboard;
    AVDexedResizeGrip resizeGrip;
    AVDexedPlayView playView;
    Label brandLabel;
    std::unique_ptr<juce::Drawable> AVBrandSymbol;
    AVDexedWheel AVPitchWheel, AVModWheel;
    Label AVPitchLabel, AVModLabel, AVOctaveLabel;
    static constexpr int AVKeyboardAnchor = 36;   // C2: the keyboard shows C2 – C7
    Label AVHelpLabel, AVCpuLabel, AVVoicesLabel;
    AVDexedPeakMeter AVOutputMeter;
    double AVLastMeterUpdate = 0.0;
    DrawableButton AVSettingsButton { "AVSettings", DrawableButton::ImageFitted };
    std::unique_ptr<Drawable> AVInfoSymbol;
    /** The editor's own background (canvas, keyboard zone, footer, brand
        block, tab separators): an image at the screen's pixels, made again
        when the keyboard, the frame's height or the tabs' places change. */
    AVDexedPixelCache AVBackgroundLayer;
    std::array<int, 4> AVBackgroundSignature {};
    void AVPaintBackground(Graphics& g);
    /** Follows Settings > Instrument > "Low power" (AVDexedTheme::lowPower). */
    void AVApplyLowPower();
    /** The first time Advanced opens in this editor (on screen, after the
        cross-fade, no other dialog up): a notice that it draws more while notes
        sound, with "Don't show again". */
    void AVMaybeShowAdvancedNotice();
    bool AVAdvancedNoticeShown = false;
    void AVUpdateFooter();
    /** The footer help text: the hovered control's help, or a temporary notice. */
    void AVUpdateHelpText();
    void AVLayoutFrame();
    void AVUpdateKeyboardRange();
    Label identityLabel;
    Label activePresetLabel;
    Label modifiedLabel;
    TextButton playTabButton { "Play" };
    TextButton advancedTabButton { "Advanced" };
    TextButton layersTabButton { "Layers" };
    AVDexedLayersView layersView;
    enum class AVView { play, advanced, layers };
    AVView AVVisibleView = AVView::play;
    Component& AVViewComponent(AVView view);
    /** The tab change's cross-fade, over the pages (AVPageArea). */
    AVDexedCrossfade AVPageFade;
    std::array<int, 4> AVPageFadeUnderSignature {};
    /** Where the pages lie in the frame (all of them together). */
    Rectangle<int> AVPageArea() const;
    /** What the frame shows under the pages, in the frame's coordinates. */
    void AVPaintUnderPages(Graphics& g);
    /** The editor's background (AVBackgroundLayer), in the editor's coordinates. */
    void AVDrawBackground(Graphics& g);
    DrawableButton saveButton { "Save", DrawableButton::ImageFitted };
    TextButton languageButton { "ES" };
    AVDexedPanicButton panicButton;

    GlobalEditor global;
    std::unique_ptr<OperatorEditor> operators[6];
    Colour background;

    CartManager cartManager;

    // This cover is used to disable main window when cart manager is shown
    Component cartManagerCover;

    std::unique_ptr<juce::Component> AVSettingsOverlay;
    // MIDI Learn of the previous / next sound arrows: the arrow waiting for its
    // CC (0 previous, 1 next, -1 none), the waiting dialog, and a token that
    // keeps a late result of an older dialog from cancelling a newer Learn.
    int AVStepLearn = -1, AVStepLearnToken = 0;
    int AVPendingSoundSteps = 0;   // pedal presses that arrived before the list was read
    Component::SafePointer<AVDexedPromptOverlay> AVStepLearnPrompt;
    void AVLearnPresetStep(int step);
    /** Learned CCs: a finished arrow Learn closes its dialog and is named in the
        footer; arrow presses walk the Play list. */
    void AVFollowPresetStepCCs();
    // Its own ticker: the editor timer stops while the bank manager is open,
    // and the arrows (in the top bar) are learned and used then too.
    juce::TimedCallback AVPresetStepPoll { [this] { AVFollowPresetStepCCs(); } };
    /** "AV Vibes" and the short rule after it (the brand block). The rule sits
        on the middle of the text's x-height, as the font's own dashes do. */
    struct AVIdentityLayout { Rectangle<float> text, rule; };
    static AVIdentityLayout AVGetIdentityLayout();
    // In-editor dialogs (new folder, import, confirmations). Finished ones are
    // kept until their result has been delivered and their fade-out has ended.
    std::vector<std::unique_ptr<AVDexedPromptOverlay>> AVPrompts;
    juce::Image AVPromptBackdrop;
    bool AVInPromptResult = false;
    bool AVPromptSweepPending = false;
    /** Releases finished prompts whose fade-out is over; looks again shortly
        while one is still fading. */
    void AVReleaseFinishedPrompts();
    // Notices (errors, import problems) wait here while another prompt or a
    // modal window is open, so two dialogs are never stacked.
    struct AVPendingNotice { juce::String title, message; };
    std::vector<AVPendingNotice> AVNoticeQueue;
    bool AVNoticePollPending = false;
    bool AVCanShowNoticeNow() const;
    void AVPumpNotices();
    /** Runs a prompt's result (or third-pill) callback: a prompt it opens reuses
        the backdrop, queued notices follow, finished prompts are released. */
    void AVDeliverPromptCallback(const std::function<void()>& callback);
    // Footer notice ("Agregado a «Pianos» · 32 sonidos"): ~3 s with fades, then
    // the normal help fades back. On the animation clock: the editor timer is
    // stopped while the bank manager is open.
    juce::String AVNoticeText;
    double AVNoticeStart = 0.0, AVHelpFadeStart = 0.0;
    AVDexedAnimation AVNoticeAnimation { *this, [this](double) {
        AVUpdateHelpText();
        return AVNoticeText.isNotEmpty() || AVHelpFadeStart > 0.0;
    } };
    // Copies 0.1.0's external banks into the library once per process.
    std::unique_ptr<juce::ThreadPool> AVAdoptionPool;
    void AVAdoptExternalSoundsOnce();
    AVDexedDropOverlay AVDropOverlay;
    // Files the system asked to open while a dialog was up: they wait (one
    // poll at a time, nothing is ever dropped) and are imported when it closes.
    StringArray AVPendingSystemFiles;
    bool AVPendingSystemPollPending = false;
    void AVOpenPendingSystemFiles();
    /** Settings is the only modal thing (a page, not a dialog). */
    bool AVOnlySettingsIsModal() const;
    /** Closes Settings: with its fade, or at once when a dialog is about to
        photograph the editor for its backdrop. */
    void AVCloseSettings(bool instantly);
    std::unique_ptr<FileChooser> fileChooser;
    int labelRefreshTicks = 0;
    int AVStatusTicks = 0;
    bool AVOperatorsSilent = false;
    float AVLastGlobalVu = -1.0f;
    int resizeSaveCountdown = 0;
    float resizeFactorAtStart = 1.0f;
    void resetSize();
    void resized() override;
    void showAVDexedPlayView(bool showPlay, bool animate = true);
    /** Play, Advanced or Layers: the GOZO cross-fade between any two. */
    void showAVView(AVView view, bool animate = true);
    void refreshPresetHeader();
    void applyLanguage();
    void changeListenerCallback(ChangeBroadcaster*) override;
    void requestPresetReplacement(std::function<void()> replacement);
    /** requestPresetReplacement for a single sound (a sound file without
        layers, a bank voice, Init): from Play or Advanced it plays alone, the
        layer mode off; on the Capas page it goes into the selected layer. */
    void requestSoundReplacement(std::function<void()> replacement);
    void commitCart(Cartridge cart, const File& file);
    void applyCart(Cartridge cart, File file);
    void storeProgramShowDialog(Cartridge destSysex, File externalFile);
    void storeProgramResponse(AlertWindow *dialog, int response, Cartridge destSysex, File externalFile, bool hasExternalFile);
    /** "Guardar": overwrites the active .avdexpreset of the library (with its
        layers when it carries them), or writes a bank voice into its own slot;
        anything else goes to "Guardar como". The completion says whether the
        sound was saved. */
    void AVSaveSound(std::function<void(bool)> completion = {});
    /** "Guardar como": a name prompt, then a new .avdexpreset in the selected
        library folder (or "User Presets"), with every layer when the sound is
        a layers preset. Never overwrites. `reason`, when given, opens the
        message (why "Guardar" could not save in place). */
    void AVSaveSoundAs(std::function<void(bool)> completion = {}, const juce::String& reason = {});
    /** "Guardar" of the Capas page: the layers' own sound file again, while it
        is still theirs; else "Guardar como" of the Capas page. */
    void AVSaveLayers(const juce::String& suggestedName);
    /** "Guardar como" of the Capas page: every layer in a new .avdexpreset. */
    void AVSaveLayersAs(const juce::String& suggestedName);
    /** Both "Guardar como"; `layers`: the file carries every layer. `folder`:
        where it goes (none: the folder being browsed, as "Guardar como"). */
    void AVSavePresetAs(bool layers, const juce::String& suggestedName, File folder,
                        std::function<void(bool)> completion, const juce::String& reason);
    enum class AVOverwrite { saved, failed, holdsOtherSound };
    /** Writes the sound (and every layer, when `layers`) over `file`, a sound
        file of the library, which then is the active one; never over a file
        that now holds a sound other than `expectedId` (renamed or deleted
        elsewhere, and a new one saved under that name). */
    AVOverwrite AVOverwritePreset(const File& file, const juce::String& expectedId, const juce::String& name, bool layers);
    /** Shows a notice and returns true when `destination` is inside a read-only pack. */
    bool AVRefuseReadOnlyDestination(const File& destination);
    /** After an import: shows `folder` (or the first sound's folder), loads the
        first valid bank (or else the first preset), then tells what was refused. */
    void AVOpenImportedSounds(const Array<File>& banks, const Array<File>& presets, const File& folder,
                              StringArray refused, StringArray failed);
    /** Banks of an import converted into sounds on the processor's thread; a
        dialog shows the progress when it takes a while (and cancels it). Then
        the folder opens with its first sound and the footer tells what was added. */
    void AVStartImportConversion(const AVDexedConversion::Request& request, Array<File> banks, Array<File> presets,
                                 StringArray refused, StringArray failed, int copied);
    void AVFinishImportConversion(const AVDexedConversion::Result& result, const File& folder, Array<File> banks,
                                  Array<File> presets, StringArray refused, StringArray failed, int copied);
    /** A conversion on the processor's thread: a dialog shows its progress
        (`progressText` of the percent done) when it takes a while, and
        cancels it; `finished` runs on the message thread. False when another
        one is running (the user is told). */
    bool AVRunConversion(const AVDexedConversion::Request& request, std::function<String(int)> progressText,
                         std::function<void(const AVDexedConversion::Result&)> finished);
    /** The library's own banks turned into sounds where they are, once: the
        question (asked by itself the first time the library is listed, or
        from "Organizar"), then the conversion, the favourites following
        their sounds and the sound playing becoming its file. */
    void AVOfferLibraryConversion(int banks);
    void AVConvertLibrary();
    /** "Organizar" › undo: every conversion of the library's own banks
        undone, newest first; the favourites go back to the bank voices. */
    void AVUndoLibraryConversion();
    /** Once the library is sound files, .syx files that appear in it (copied
        with Finder, the factory banks restored) become sounds too. */
    void AVConvertBanksAdded(const Array<File>& banks);
    /** "Organizar" › export: `sounds` as .syx banks wherever the user chooses. */
    void AVExportSounds(const Array<File>& sounds, const String& name);
    /** After a conversion of the library's own banks: favourites, the sound playing, the browser. */
    void AVAdoptConverted(const AVDexedConversion::Result& result);
    std::set<String> AVBanksTried;   // AVConvertBanksAdded: files tried (path|time), never again
    Component::SafePointer<AVDexedPromptOverlay> AVConversionPrompt;
    void loadNativePreset(const File& file);
public:
    DexedAudioProcessor *processor;

    DexedAudioProcessorEditor (DexedAudioProcessor* ownerFilter);
    ~DexedAudioProcessorEditor();
    virtual void timerCallback() override;

    virtual void paint (Graphics& g) override;
    virtual void comboBoxChanged (ComboBox* comboBoxThatHasChanged) override;
    void updateUI();
    void rebuildProgramCombobox();
    void loadCart(File file);
    void saveCart();
    void initProgram();
    void storeProgram();
    void cartShow();
    void parmShow();
    /** Settings (blurred, fixed, in the editor) open on `page`; an open one
        turns to it. "AV Dexed" in Advanced opens it on About. */
    void showAVSettingsPage(AVDexedSettingsPanel::AVPage page);
    void tuningShow();
    /** Shows an in-editor dialog (not movable, blurred backdrop). A dialog opened
        from another dialog's result reuses the first backdrop. Returns it (owned
        here; a SafePointer to it goes null once it has faded out). */
    AVDexedPromptOverlay* showAVPrompt(AVDexedPromptOverlay::Options options, AVDexedPromptOverlay::Result result);
    /** MIDI Learn of `parameter` from `control`'s menu: the in-editor waiting
        dialog ("Aprender MIDI", a single Cancel). The first CC that arrives
        closes it and the footer names it. */
    void showAVMidiLearn(Component& control, RangedAudioParameter& parameter);
    bool hasAVPromptOpen() const;
    /** An in-editor notice with a single OK pill. Queued while another prompt
        or modal window is open; shown when it closes. */
    void showAVNotice(const juce::String& title, const juce::String& message);
    /** A short confirmation in the footer for about 3 s, then the normal help. */
    void showAVFooterNotice(const juce::String& text);
    /** Writes `bank` into a .syx under the library lock, as every write another
        AV Dexed may race with: a new file, or an existing one whose bank message
        alone is replaced (its other SysEx messages and embedded sound settings
        stay). Refuses an existing file of 64 KB or more (an archive of several
        banks, which a rewrite would cut) and one larger than a bank that holds
        no bank. With `storedSlot` (0–31), that slot's embedded settings become
        `storedSettings` (empty = defaults) while every other slot keeps playing
        as before, the same rule as "Guardar". */
    static bool AVWriteBankFile(Cartridge bank, const File& file, int storedSlot = -1,
                                const std::map<juce::String, float>& storedSettings = {});

    static float getLargestScaleFactor();
    void resetZoomFactor();

    virtual bool isInterestedInFileDrag (const StringArray &files) override;
    virtual void filesDropped (const StringArray &files, int x, int y ) override;
    void fileDragEnter(const StringArray& files, int x, int y) override;
    void fileDragExit(const StringArray& files) override;
    /** Adds .syx banks, .dexedpreset sounds and folders of them (recursively) to
        the library: files already inside it stay where they are (no prompt when
        all of them are); the rest are copied, never overwriting, into the folder
        named in a prompt. Then that folder is shown and the first bank (or
        preset) loads. Used by "Importar banco", drag and drop and the Advanced
        bank manager. */
    void importBanks(const Array<File>& files);
    /** Files the system asks the app to open (a double-click in the Finder, a
        drop on the Dock icon, "Open with"): the same import as a drop on the
        window. While a dialog is open (an earlier import's question, "save
        changes") they wait and are imported once it closes. */
    void AVOpenFilesFromSystem(const StringArray& paths);
    std::unique_ptr<ComponentTraverser> createFocusTraverser() override;
    std::unique_ptr<ComponentTraverser> createKeyboardFocusTraverser() override;
    AVDexedAnimationClock& getAnimationClock() override { return AVClock; }

    bool keyPressed(const KeyPress& key, Component* originatingComponent) override;

    /** A mouse click hides focus rings until the next keyboard navigation. */
    struct AVFocusModality final : public MouseListener {
        void mouseDown(const MouseEvent& event) override;
    } focusModality;

    static constexpr float AVTopBarAxis = 47.0f;  // shared centre line of the top bar
    static const int WINDOW_SIZE_X = 1440;
    static const int WINDOW_SIZE_Y = 940;
#if JUCE_WINDOWS
    static constexpr float AVMinZoom = 0.60f;   // 1440x940 at 0.8 does not fit 1080p at 150 % scaling
#else
    static constexpr float AVMinZoom = 0.80f;
#endif

#if defined(AVDEXED_TESTING)
    friend bool AVRenderUIEvidence(const juce::File& directory);
    friend bool AVRunLibraryQA(const juce::File& outputDirectory);
    friend bool runAVDexedDropTests();
    friend int AVRunIndexBench(const juce::File&, bool);
    friend int AVRunUICpu(const juce::File& bankFile, int slot);
    friend int AVRunUIResize(const juce::File& bankFile, int slot, const juce::File& outputDirectory);
    friend int AVRunAdvancedNotice(const juce::File& outputDirectory);
    friend int AVRunUIFades(const juce::File& bankFile, int slot);
    friend int AVRunUIFadesCheck(const juce::File& bankFile, int slot, const juce::File& outputDirectory);
    friend int AVRunUIStress(const juce::File& bankFile, int slot, int minutes);
    friend int AVRunUISounds(const juce::File& bankFile, int slot);
#endif

private:
    friend class CartManager;
#ifdef DEXED_EVENT_DEBUG
    FocusLogger focusLogger;
#endif
};


#endif  // PLUGINEDITOR_H_INCLUDED
