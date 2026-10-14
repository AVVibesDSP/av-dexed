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
#include "parameter/DexedApvts.h"
#include "core/Program.h"
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
    Lfo lfo;

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
    std::atomic<float> avAudioCpuPercent { 0.0f };
    std::atomic<float> avOutputPeakL { 0.0f }, avOutputPeakR { 0.0f };
    std::atomic<int> avActiveVoices { 0 };
    void finishAVAudioMeter(juce::int64 startTicks, int samples) noexcept;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterVolumeSmoother;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> panSmoother;
    
    void processMidiMessage(const MidiMessage *msg);
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
#if defined(AVDEXED_TESTING)
    struct TestMode {};
    explicit DexedAudioProcessor(TestMode);
    void flushAVPendingSysExForTesting() { flushAVPendingSysEx(); }
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
    void setAVKeyboardPitch(int value) noexcept { avKeyboardPitchPending.store(juce::jlimit(0, 16383, value), std::memory_order_relaxed); }
    void setAVKeyboardMod(int value) noexcept { avKeyboardModPending.store(juce::jlimit(0, 127, value), std::memory_order_relaxed); }
    int getAVKeyboardPitch() const noexcept { return avKeyboardPitch.load(std::memory_order_relaxed); }
    int getAVKeyboardMod() const noexcept { return avKeyboardMod.load(std::memory_order_relaxed); }
    float getAVAudioCpuPercent() const noexcept { return avAudioCpuPercent.load(std::memory_order_relaxed); }
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
    int32_t nextKeydownSeq;;
    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DexedAudioProcessor)

};

#endif  // PLUGINPROCESSOR_H_INCLUDED
