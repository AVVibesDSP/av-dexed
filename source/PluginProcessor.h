/**
 *
 * Copyright (c) 2013-2025 Pascal Gauthier.
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

#ifndef PLUGINPROCESSOR_H_INCLUDED
#define PLUGINPROCESSOR_H_INCLUDED

#include "../JuceLibraryCode/JuceHeader.h"

#include "clap-juce-extensions/clap-juce-extensions.h"

#include "msfa/controllers.h"
#include "msfa/dx7note.h"
#include "msfa/lfo.h"
#include "msfa/synth.h"
#include "msfa/fm_core.h"
#include "msfa/tuning.h"
#include "PluginData.h"
#include "PluginFx.h"
#include "effects/AVDexedFxChain.h"
#include "midi/SysexComm.h"
#include "engine/EngineMkI.h"
#include "engine/EngineOpl.h"
#include "engine/AVDexedLayer.h"
#include "parameter/DexedApvts.h"
#include "core/Program.h"
#include "core/AVDexedLibrary.h"
#include "core/AVDexedPerformance.h"
#include "util/CommandFifo.h"

struct ProcessorVoice {
    int channel;
    int midi_note;
    int velocity;
    bool keydown;
    bool sustained;
    bool live;
    int32_t keydown_seq;

    int mpePitchBend;
    Dx7Note *dx7_note;
};

enum DexedEngineResolution {
    DEXED_ENGINE_MODERN,
    DEXED_ENGINE_MARKI,
    DEXED_ENGINE_OPL
};

/// Maximum allowed size for SCL and KBM files.
/// (COMMENT: Since none of the 5175 .scl files in the Scala archive
/// at https://www.huygens-fokker.org/scala/downloads.html#scales
/// exceed 6 KB (in 25th Mar 2024), a maximum size of 16 KB appears
/// to be a practical choice.)
const int MAX_SCL_KBM_FILE_SIZE = 16384;

class MidiCCHandler;

//==============================================================================
/**
*/
class DexedAudioProcessor  : public AudioProcessor, public MidiInputCallback, public clap_juce_extensions::clap_properties, private juce::Timer
{
    enum class SetupMode { userData, isolatedTest };
    explicit DexedAudioProcessor(SetupMode setupMode);

    static const int MAX_ACTIVE_NOTES = 16;
    ProcessorVoice voices[MAX_ACTIVE_NOTES];
    int currentNote;

    // The original DX7 had one single LFO. Later units had an LFO per note.
    // AV Dexed: value-initialised (phase, delay and sample-and-hold state
    // start at zero instead of whatever the memory held).
    Lfo lfo {};

    bool sustain;
    bool monoMode;
    
    // Extra buffering for when GetSamples wants a buffer not a multiple of N
    float extra_buf[N];
    int extra_buf_size;

    int currentProgram;
    
    /**
     * Plugin fx (the filter)
     */
    PluginFx fx;
    AVDexedFxChain effects;
    std::atomic<bool> fxResetRequested { false };
    juce::AudioBuffer<float> stereoFxBuffer;

    /**
     * This flag is used in the audio thread to know if the voice has changed
     * and needs to be updated.
     */
    juce::Atomic<bool> refreshVoice { false };

    bool normalizeDxVelocity;
    bool sendSysexChange;
    std::atomic<float> masterVolumeValue { 1.0f };
    std::atomic<float> panValue { 0.5f };
    std::atomic<float> avDelayTempoValue { 120.0f };
    std::atomic<int> avKeyboardPitch { 8192 }, avKeyboardMod { 0 };
    std::atomic<int> avKeyboardPitchPending { -1 }, avKeyboardModPending { -1 };
    std::atomic<bool> avPanicRequested { false };
    std::atomic<bool> avMonoResetRequested { false };
    static constexpr int AVSysExQueueSize = 8, AVSysExPacketSize = 4104;
    struct AVSysExPacket { std::array<uint8_t, AVSysExPacketSize> bytes {}; int size = 0; std::atomic<size_t> sequence { 0 }; };
    std::array<AVSysExPacket, AVSysExQueueSize> avSysExPackets;
    std::atomic<size_t> avSysExWrite { 0 };
    size_t avSysExRead = 0;
    std::atomic<int> avDroppedSysEx { 0 };
    static constexpr int AVMidiOutputQueueSize = 8, AVMidiOutputBatchBytes = 65536;
    struct AVMidiOutputBatch { juce::MidiBuffer buffer; std::atomic<int> state { 0 }; };
    std::array<AVMidiOutputBatch, AVMidiOutputQueueSize> avMidiOutputBatches;
    int avMidiOutputRead = 0, avMidiOutputWrite = 0;
    bool enqueueAVSysEx(const uint8_t* data, int size) noexcept;
    void handleAVSysExNow(const uint8_t* data, int size);
    void flushAVPendingSysEx();
    void timerCallback() override;
    /** Timer, with no editor open: moves through the current bank by the steps
        the learned previous / next CCs asked for (see consumeAVPresetSteps). */
    void stepAVPresetsWithoutEditor();
    std::atomic<float> avAudioCpuPercent { 0.0f };
    std::atomic<float> avOutputPeakL { 0.0f }, avOutputPeakR { 0.0f };
    std::atomic<int> avActiveVoices { 0 };
    void finishAVAudioMeter(juce::int64 startTicks, int samples) noexcept;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterVolumeSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> panSmoother;
    
    void processMidiMessage(const MidiMessage *msg);
    /** Layer mode: a message for the layers that listen to its channel. */
    void processAVLayerMidi(const MidiMessage& message);
    /** Layer mode: the engine blocks of every layer, mixed to stereo by each
        layer's volume and pan. Same block bookkeeping as the single path. */
    void renderAVLayers(float* left, float* right, int numSamples, MidiBufferIterator& it, MidiBufferIterator& end);
    void keydown(uint8_t chan, uint8_t pitch, uint8_t velo);
    void keyup(uint8_t, uint8_t pitch, uint8_t velo);

    /**
     * Process midi messages until the samplePos is reached.
     */
    void processMidiMessages(MidiBufferIterator& cur, MidiBufferIterator& end, int samplePos);

    void handleIncomingMidiMessage(MidiInput* source, const MidiMessage& message) override;
    uint32_t engineType;
    
    FmCore engineMsfa;
    EngineMkI engineMkI;
    EngineOpl engineOpl;

    void resolvAppDir();

    float zoomFactor = 1;

    void mapParameters();
    void setDxValue(int offset, int v);

    /**
     * Apply a DX7 parameter change (by sysex offset) through the matching APVTS
     * parameter, so the model/voice and the bound UI control stay in sync.
     * Offsets without a dedicated parameter fall back to a direct model write.
     */
    void setDxParameter(int offset, uint8_t value);

    void valueTreeChildAdded(ValueTree& parentTree, ValueTree& childWhichHasBeenAdded);
public :
    static constexpr float AVMasterMaximumGain = 15.848932f; // +24 dB; 0 remains exact mute.

    // LAYERS (X816 mode) ==============================================
    // With the layer mode off the instrument plays as always (the same code,
    // the same samples). With it on, up to eight layers play, each a whole
    // DX7 (AVDexedLayer) with its own voice, MIDI channel, key range,
    // transposition, tuning, volume and pan. The editor edits the selected
    // layer's voice: activeProgram is that voice.
    static constexpr int AVLayerCount = 8;
    struct AVLayerSettings {
        std::atomic<bool> enabled { false };
        std::atomic<int> channel { 0 };        // 0: every channel, else 1-16
        std::atomic<int> lowNote { 0 };
        std::atomic<int> highNote { 127 };
        std::atomic<int> transpose { 0 };      // semitones, -24..24
        std::atomic<float> detune { 0.0f };    // cents, -50..50
        std::atomic<float> volume { 1.0f };    // gain, 0..1
        std::atomic<float> pan { 0.5f };       // 0 left, 0.5 centre, 1 right
        std::atomic<int> alternateGroup { -1 }; // TX802 alternate assign: the layers of one group take turns (-1: none)
        // DX7II voice functions (from a performance's voices): 0 = off.
        std::atomic<float> unison { 0.0f };    // two voices per key, ± these cents
        std::atomic<float> randomPitch { 0.0f };   // each note tuned at random within ± these cents
        std::atomic<int> mono { -1 };          // -1: as the instrument, 0: poly, 1: mono
        // A DX7II preset micro tuning for this layer (-1: the instrument's tuning).
        std::atomic<int> microTable { -1 };
        std::atomic<int> microKey { 0 };
        // Free phase (separate modules of a rack): where this layer's notes start,
        // in turns of their fundamental, not in step with the other layers'. -1: off.
        std::atomic<float> phase { -1.0f };
    };
    static constexpr float AVLayerMaxDetune = 50.0f;
    bool isAVLayerModeOn() const noexcept { return avLayerMode.load(std::memory_order_acquire); }
    /** Message thread. Turning it on puts the current sound in the selected
        layer (enabled when no layer is); turning it off keeps that sound. */
    void setAVLayerMode(bool on);
    int getAVSelectedLayer() const noexcept { return avSelectedLayer.load(std::memory_order_acquire); }
    /** Message thread: the editor now edits `layer`'s voice (its edits so far
        stay with the layer it leaves). The other layers keep playing. */
    void selectAVLayer(int layer);
    AVLayerSettings& getAVLayerSettings(int layer) noexcept { return avLayerSettings[size_t(juce::jlimit(0, AVLayerCount - 1, layer))]; }
    /** The voice of `layer` (for the selected one, the voice being edited). */
    Program getAVLayerProgram(int layer) const;
    /** Message thread: `layer` plays `voice` (the selected one also loads it
        into the editor). */
    void setAVLayerProgram(int layer, const Program& voice);
    int getAVLayerSoundingVoices(int layer) const noexcept { return avLayerVoices[size_t(juce::jlimit(0, AVLayerCount - 1, layer))].load(std::memory_order_relaxed); }
    /** Unison, random pitch and mono of `layer` from a voice's AMEM (35 bytes);
        nullptr: none (the DX7's behaviour). */
    void setAVLayerVoiceExtras(int layer, const uint8_t* amem35);
    /** Message thread: `layer` plays DX7II preset micro tuning `table` (1-10)
        on `key` (0 = C); -1 or 0: the instrument's own tuning. */
    void setAVLayerMicroTuning(int layer, int table, int key);
    /** Free phase on: `layer` plays as a separate module of a rack, its notes
        not in step with the other layers' (at the start an eight-layer stack
        gives it); off: in step, as one instrument. */
    void setAVLayerFreePhase(int layer, bool on);
    /** Message thread. Turning a layer off fades it out and stops its notes. */
    void setAVLayerEnabled(int layer, bool enabled);
    /** Message thread: the layer listens to `channel` (0: every channel). Its
        pedal, pitch bend and wheels go back to rest: the old channel's
        pedal-up would never reach it. */
    void setAVLayerChannel(int layer, int channel);
    /** Message thread: the selected voice in `count` layers spread like a
        X816 ensemble (alternating pan, symmetric detune of `cents`). */
    void stackAVLayers(int count, float cents);
    /** Message thread: a whole set of layers at once (a DX7II or TX802
        performance). The first layer that sounds becomes the selected one. */
    void applyAVLayerSetup(const std::array<AVDexedPerformance::ResolvedLayer, AVLayerCount>& layers);
    /** A layers file (.avdexlayers): every layer with its voice, as XML. */
    juce::String createAVLayersFile(const juce::String& name) const;
    /** Message thread: plays a layers file; false when it is not one. */
    bool loadAVLayersFile(const juce::String& text);
    static constexpr const char* AVLayersExtension = ".avdexlayers";
    /** Message thread: every layer back to its defaults (off, every channel,
        whole keyboard, no transposition or detune, full volume, centre). */
    void resetAVLayers();
    /** Silences the voices a sound change affects: everything with one layer,
        only the selected layer in layer mode. */
    void panicForSoundChange();
    /** True while a sound file (not a session) is being restored. */
    bool isAVLoadingSoundFile() const noexcept { return avLoadingSoundFile; }
    /** Render: the selected layer takes a sound change at once (a program
        change in the middle of a block). */
    void applySoundChangeNow();
#if defined(AVDEXED_TESTING)
    struct TestMode {};
    explicit DexedAudioProcessor(TestMode);
    void flushAVPendingSysExForTesting() { flushAVPendingSysEx(); }
    void runAVTimerTickForTesting() { timerCallback(); }
#endif
    DexedApvts parameters;
    Program activeProgram;
    // Musical preset context belongs to the processor, so destroying an editor
    // cannot discard a Unicode name, origin, or the loaded-content reference.
    juce::String getAVPresetName() const;
    juce::String getAVPresetId() const;
    juce::File getAVPresetPath() const;
    bool hasAVKnownPresetOrigin() const noexcept { return avKnownPresetOrigin.load(std::memory_order_relaxed); }
    void setAVPresetContext(const juce::String& name, const juce::String& id, const juce::File& file);
    void captureAVPresetBaseline() noexcept;
    bool isAVPresetModified() const noexcept;
    juce::MemoryBlock createAVNativePresetState(const juce::String& name, const juce::String& id);
    bool loadAVStateInformation(const void* data, int sizeInBytes);
    /** Loads a .dexedpreset. Unlike a host session, a sound file never changes
        the instrument-wide settings: "Volumen general" and the local tempo keep
        their live values. */
    bool loadAVNativePreset(const void* data, int sizeInBytes);
    void setAVKeyboardPitch(int value) noexcept { avKeyboardPitchPending.store(juce::jlimit(0, 16383, value), std::memory_order_relaxed); }
    void setAVKeyboardMod(int value) noexcept { avKeyboardModPending.store(juce::jlimit(0, 127, value), std::memory_order_relaxed); }
    int getAVKeyboardPitch() const noexcept { return avKeyboardPitch.load(std::memory_order_relaxed); }
    int getAVKeyboardMod() const noexcept { return avKeyboardMod.load(std::memory_order_relaxed); }
    float getAVAudioCpuPercent() const noexcept { return avAudioCpuPercent.load(std::memory_order_relaxed); }
    /** Whether either channel peaked at `level` or more since the last consume. */
    bool hasAVOutputPeak(float level) const noexcept {
        return avOutputPeakL.load(std::memory_order_relaxed) >= level || avOutputPeakR.load(std::memory_order_relaxed) >= level;
    }
    std::pair<float, float> consumeAVOutputPeaks() noexcept {
        return { avOutputPeakL.exchange(0.0f, std::memory_order_relaxed),
                 avOutputPeakR.exchange(0.0f, std::memory_order_relaxed) };
    }
    int getAVActiveVoices() const noexcept { return avActiveVoices.load(std::memory_order_relaxed); }
    int getAVDroppedSysExCount() const noexcept {
        return avDroppedSysEx.load(std::memory_order_relaxed) + sysexComm.getAVDroppedOutputCount();
    }
    std::unique_ptr<MidiCCHandler> midiCCMapper;

    // in MIDI units (0x4000 is neutral)
    Controllers controllers;
    StringArray programNames;
    Cartridge currentCart;

    SysexComm sysexComm;
    VoiceStatus voiceStatus;
    /** The file of the active bank (empty for a bank without file, e.g. received
        by SysEx). Read it freely; change it only through setActiveBank(). */
    File activeFileCartridge;

    /**
     * Set when the current program or cartridge changes from a non-UI source
     * (host program change, incoming sysex dump). The editor polls this to
     * rebuild the program combobox / active-program label, which are not APVTS
     * parameters and therefore don't refresh through parameter attachments.
     */
    juce::Atomic<bool> programChanged { false };

    float vuSignal;
    double vuDecayFactor = 0.999361; // (for 48 kHz sampling rate)
    bool showKeyboard;
    int getEngineType();
    void setEngineType(int rs);

    void loadCartridge(const Cartridge& cart);

    /** The bank the browser, the program list and "Guardar" refer to: loads
        `bank` as the current cartridge, remembers `file` (empty for a bank that
        arrived by SysEx or has no file) and reloads the per-sound settings
        embedded in that file. The program and every parameter stay as they are.
        The only place that changes activeFileCartridge. Not on the audio thread. */
    void setActiveBank(const Cartridge& bank, const juce::File& file);

    // MIDI learn for the previous / next sound arrows. A learned CC steps through
    // the sounds when it goes from below 64 to 64 or above (a button or pedal
    // press). Kept with the MIDI CC assignments (session and MIDI profiles).
    enum class AVPresetStep { previous = 0, next = 1 };
    /** The next CC that arrives is learned for `step`. Message thread. */
    void startAVPresetStepLearn(AVPresetStep step);
    void cancelAVPresetStepLearn();
    /** The CC learned for `step`, or -1. */
    int getAVPresetStepCC(AVPresetStep step) const noexcept;
    void clearAVPresetStepCC(AVPresetStep step);
    /** The step whose learn just finished (0 previous, 1 next), or -1. Message thread. */
    int consumeAVPresetStepLearned() noexcept;
    /** Net steps asked for by learned CCs since the last call (+1 per "next"
        press, -1 per "previous"). The open editor consumes them (it walks the
        Play view's list); with no editor the processor steps through the bank. */
    int consumeAVPresetSteps() noexcept;

    enum class AVBankSaveResult { saved, needsSaveAs, failed };
    /** True when "Guardar" can write the current sound into its own slot of the
        active bank: the file exists inside the library, is not in a read-only
        pack, can be written, and the processor has a bank loaded from it.
        (Whether the file still holds that bank is checked again when saving.) */
    bool canSaveAVSoundToBank() const;
    /** Writes the current voice (renamed to `name`, at most 10 ASCII characters
        with accents folded; an empty name or the name it already has keeps the
        voice's own) and its modern settings into the current slot of the active bank file,
        holding the library lock: re-reads the file, checks it still holds the
        loaded bank (else needsSaveAs), patches only that slot and its settings,
        writes atomically. On success the bank is reloaded, the program stays,
        and the baseline is recaptured. Message thread. */
    AVBankSaveResult saveAVSoundToBank(const juce::String& name);
    /** The current sound's modern settings (volume, pan, filter, reverb, delay,
        chorus): values that differ from the default, in real units. */
    AVDexedLibrary::SlotSettings captureAVSoundSettings() const;
    bool hasExternalCartridgeContext() const noexcept {
        return activeFileCartridge.getFullPathName().isNotEmpty();
    }

    //==============================================================================
    DexedAudioProcessor();
    ~DexedAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (AudioSampleBuffer& buffer, MidiBuffer& midiMessages) override;
    void panic();
    void requestAVPanic() noexcept { avPanicRequested.store(true, std::memory_order_relaxed); }
    void resetMonoMode();
    
    void copyToClipboard(int srcOp);
    void pasteOpFromClipboard(int destOp);
    void pasteEnvFromClipboard(int destOp);
    void sendCurrentSysexProgram();
    void sendCurrentSysexCartridge();
    void sendSysexCartridge(File cart);
    
    //==============================================================================
    AudioProcessorEditor* createEditor() override;
    /** Called by the editor as it opens and closes: with no editor, learned
        arrow presses step through the bank in render. */
    void setAVEditorOpen(bool open) noexcept { avEditorOpen.store(open, std::memory_order_release); }
    bool hasEditor() const override;
    bool peekVoiceStatus();
    int updateProgramFromSysex(const uint8 *rawdata);
    void setupStartupCart();
    
    //==============================================================================
    const String getName() const override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    void setCurrentProgramSelection(int index);
    const String getProgramName (int index) override;
    void changeProgramName(int index, const String& newName) override;
    void resetToInitVoice() ;
    
    //==============================================================================
    void getStateInformation (MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    
    // this is kept up to date with the midi messages that arrive, and the UI component
    // registers with it so it can represent the incoming messages
    MidiKeyboardState keyboardState;

    void loadPreference();
    void savePreference();
    
    static File dexedAppDir;
    static File dexedCartDir;
    /** AV Dexed's preferences (language, zoom, favourites, browser state). It was
        called "Dexed.xml" until 0.1.0; the old file is migrated by copy. */
    static constexpr const char* preferencesFileName = "AV Dexed Preferences.xml";
    static File getPreferencesFile() { return dexedAppDir.getChildFile(preferencesFileName); }
    int lastActiveVoice = 0;

    void setZoomFactor(float factor);
    float getZoomFactor() {
        return zoomFactor;
    }

    CommandFifo<DexedAudioProcessor> command;

    // Message-thread producer; render consumes prepared buffers by swapping
    // their storage into the host buffer, without constructing MIDI messages.
    bool queueAVMidiOutputBatch(const juce::MidiBuffer& messages);

    void applyProgram(const Program &program);

    // TUNING STUFF ====================================================
    MTSClient *mtsClient;
    std::shared_ptr<TuningState> synthTuningState;

    // holds the previous working tuning state;
    // used to restore tuning state when there was a problem
    // with loading/applying a new .SCL and/or .KBM file
    std::shared_ptr<TuningState> synthTuningStateLast;

    // Prompt for a file
    void applySCLTuning();
    void applyKBMMapping();
    std::unique_ptr<FileChooser> tuningFileChooser;

    // Load a file
    void applySCLTuning(File sclf);
    void applyKBMMapping(File kbmf);

    // Load from text
    void applySCLTuning(std::string scld);
    void applyKBMMapping(std::string kbmd);

    void retuneToStandard();
    void resetTuning(std::shared_ptr<TuningState> t);
    int tuningTranspositionShift();
    /** The shift for a voice whose transpose (byte 144) is `transpose`: each layer uses its own voice's. */
    int tuningTranspositionShift(int transpose);

    std::string currentSCLData = "";
    std::string currentKBMData = "";
    // TUNING STUFF ====================================================
private:
    uint64_t getAVMusicalFingerprint(const juce::String* nativeName = nullptr) const noexcept;
    juce::String avPresetName;
    juce::String avPresetId;
    juce::File avPresetPath;
    std::atomic<bool> avPresetContextCurrent { false };
    std::atomic<bool> avKnownPresetOrigin { true };
    std::atomic<uint64_t> avPresetBaseline { 0 };
    // Version of getAVMusicalFingerprint()'s recipe, saved with the baseline.
    // 3: the parameter the mod wheel drives is left out, and lists saved
    // before CC1 → Cutoff get it on restore (an older baseline cannot match).
    static constexpr int avPresetBaselineFormat = 4;
    // True while loadAVNativePreset restores a sound file: its mapping list
    // replaces the live one, but the sound arrows' CCs stay as they are.
    // Message thread only.
    bool avLoadingSoundFile = false;

    // Per-sound modern settings of the active bank (volume, pan, filter and the
    // three effects), read from the settings embedded in its .syx. Program
    // changes arrive on the audio thread (MIDI, host program parameter), so the
    // table is prepared on the message thread and only copied in under the spin
    // lock; the reader copies one slot out. No file access, allocation or
    // blocking lock on the audio thread.
    static constexpr int AVSoundParameterCount = 24;
    struct AVSoundSlot {
        juce::uint64 voice = 0;          // hash of the packed voice the settings belong to
        bool hasSettings = false;
        std::array<float, AVSoundParameterCount> values {};   // normalised targets (defaults where not stored)
    };
    using AVSoundTable = std::array<AVSoundSlot, 32>;
    std::array<juce::RangedAudioParameter*, AVSoundParameterCount> avSoundParameters {};
    // Instrument-wide settings: never part of a sound, never "Modified".
    juce::RangedAudioParameter* avOutputParameter = nullptr;
    juce::RangedAudioParameter* avDelayTempoParameter = nullptr;
    // False after Init or a single voice received by SysEx: the edit buffer no
    // longer holds the bank slot's sound, so "Guardar" must not write over it.
    std::atomic<bool> avSoundFromBankSlot { true };
    // Whether an editor is open, for the audio thread (getActiveEditor() is not
    // for that thread): learned arrow presses with no editor step in render.
    std::atomic<bool> avEditorOpen { false };
    std::atomic<bool> avHostProgramDirty { false };
    AVSoundTable avSoundTable {};
    juce::SpinLock avSoundTableLock;
    void resolveAVSoundParameters();
    AVSoundTable makeAVSoundTable(const AVDexedLibrary::BankSettings& settings) const;
    AVSoundTable readAVSoundTable(const juce::File& bankFile) const;
    void installAVActiveBank(const Cartridge& bank, const juce::File& file, const AVSoundTable& table);
    void applyAVSoundSettings(int program);
    /** Packs `voice` into `slot` of `bank` only when it differs from what the
        slot holds (same rule for "Guardar" and captureAVSoundSettings()). */
    static bool packAVVoiceIfChanged(Cartridge& bank, int slot, const Program& voice);

    // Hosts may call setCurrentProgram(restored) right after setStateInformation;
    // the session already holds that sound's settings, so that one call keeps
    // them. Armed by a host restore, consumed by the next program change or
    // expired by the 30 Hz timer after about a second. It only holds for the
    // restored bank (its voices' hash): the same slot of another bank (a click
    // in the browser, a SysEx bank) gets its own settings.
    static constexpr int AVRestoreGuardTicks = 30;
    std::atomic<int> avRestoreGuardProgram { -1 };
    std::atomic<juce::uint64> avRestoreGuardBank { 0 };
    std::atomic<int> avRestoreGuardTicksLeft { 0 };
    void armAVRestoreGuard() noexcept;
    void tickAVRestoreGuard() noexcept;

    int32_t nextKeydownSeq;;

    // Layer mode state. The settings are written by the editor and read in
    // render; the layers themselves only ever change in render or under the
    // callback lock.
    std::array<AVDexedLayer, AVLayerCount> avLayers;
    std::array<AVLayerSettings, AVLayerCount> avLayerSettings;
    std::atomic<bool> avLayerMode { false };
    std::atomic<int> avSelectedLayer { 0 };
    std::atomic<uint32_t> avLayerPanicMask { 0 };
    std::atomic<uint32_t> avLayerReleaseMask { 0 };   // layers whose pedal, bend and wheels go back to rest (a new channel)
    std::atomic<uint32_t> avLayerFadeStopMask { 0 };  // layers turned off: their notes stop once the volume has faded
    std::array<std::atomic<int>, AVLayerCount> avLayerVoices {};
    bool avLayerModeRendered = false;          // render: the mode of the last block
    std::array<unsigned, AVLayerCount> avAlternateTurn {};   // render: whose turn it is in each alternate group
    // Render: the pitch each layer gave to a note still held, by channel and
    // key, so its note-off reaches the same voice after a change of transposition.
    std::array<std::array<int8_t, 16 * 128>, AVLayerCount> avLayerHeld {};
    // How many note-ons of that channel and key the layer still holds (a key
    // played again before its note-off): each note-off releases one.
    std::array<std::array<uint8_t, 16 * 128>, AVLayerCount> avLayerHeldCount {};
    void clearAVLayerHeld(int layer) noexcept {
        avLayerHeld[size_t(layer)].fill(-1);
        avLayerHeldCount[size_t(layer)].fill(0);
    }
    /** Render: the instrument's function settings, this layer's tuning, mono
        and voice functions into the layer (once per block, and at once after
        a program change in the middle of one). */
    void syncAVLayer(int layer);
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>, AVLayerCount> avLayerGainLeft, avLayerGainRight;
    std::array<std::shared_ptr<TuningState>, AVLayerCount> avLayerTuning {};   // a layer's own micro tuning, or none
    std::shared_ptr<TuningState> tuningForAVLayer(int layer) const { return avLayerTuning[size_t(layer)] ? avLayerTuning[size_t(layer)] : synthTuningState; }
    PluginFx fxRight;                           // the filter for the right channel in layer mode
    juce::AudioBuffer<float> avLayerScratch;    // the right channel for a mono output
    float avLayerExtraLeft[N] {}, avLayerExtraRight[N] {};
    int avLayerExtraSize = 0;
    void loadSelectedLayerIntoEditor(const Program& voice);
    /** The layers in a host session: mode, selected layer, and each layer's
        settings and voice. A sound file (.avdexpreset) never carries them. */
    juce::ValueTree createAVLayersState() const;
    /** From a session; an older session (no layers) restores one layer. */
    void restoreAVLayersState(const juce::ValueTree& layers);
    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DexedAudioProcessor)

};

#endif  // PLUGINPROCESSOR_H_INCLUDED
