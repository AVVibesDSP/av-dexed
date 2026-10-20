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

#include <stdarg.h>
#include <bitset>

#include "PluginProcessor.h"
#include <cstring>
#include "ui/PluginEditor.h"
#include "parameter/Model.h"

#include "debugger/Logger.h"
#include "msfa/synth.h"
#include "msfa/freqlut.h"
#include "msfa/sin.h"
#include "msfa/exp2.h"
#include "msfa/env.h"
#include "msfa/pitchenv.h"
#include "msfa/porta.h"
#include "msfa/aligned_buf.h"
#include "msfa/fm_op_kernel.h"

#include "midi/MidiCCHandler.h"
#include "debugger/MemoryLogger.h"

#if JUCE_MSVC
    #pragma comment (lib, "kernel32.lib")
    #pragma comment (lib, "user32.lib")
    #pragma comment (lib, "wininet.lib")
    #pragma comment (lib, "advapi32.lib")
    #pragma comment (lib, "ws2_32.lib")
    #pragma comment (lib, "version.lib")
    #pragma comment (lib, "shlwapi.lib")
    #pragma comment (lib, "winmm.lib")
	#pragma comment (lib, "DbgHelp.lib")
	#pragma comment (lib, "Imm32.lib")

	#ifdef _NATIVE_WCHAR_T_DEFINED
		#ifdef _DEBUG
			#pragma comment (lib, "comsuppwd.lib")
		#else
			#pragma comment (lib, "comsuppw.lib")
		#endif
	#else
		#ifdef _DEBUG
			#pragma comment (lib, "comsuppd.lib")
		#else
			#pragma comment (lib, "comsupp.lib")
		#endif
	#endif

#endif

//==============================================================================
DexedAudioProcessor::DexedAudioProcessor()
    : DexedAudioProcessor(SetupMode::userData) {}

#if defined(AVDEXED_TESTING)
DexedAudioProcessor::DexedAudioProcessor(TestMode)
    : DexedAudioProcessor(SetupMode::isolatedTest) {}
#endif

DexedAudioProcessor::DexedAudioProcessor(SetupMode setupMode)
    : AudioProcessor(BusesProperties().withOutput("output", AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr) {

    MemoryLogger::getInstance()->install();

    DXLOG("     _                   _ ");
    DXLOG("  __| | _____  _____  __| |");
    DXLOG(" / _` |/ _ \\ \\/ / _ \\/ _` |");
    DXLOG("| (_| |  __/>  <  __/ (_| |");
    DXLOG(" \\__,_|\\___/_/\\_\\___|\\__,_|");
    DXLOG("VERSION " DEXED_VERSION);

    mapParameters();
    // Before the startup bank and program: setCurrentProgram applies their settings.
    resolveAVSoundParameters();

    Exp2::init();
    Tanh::init();
    Sin::init();

    synthTuningState = createStandardTuning();
    synthTuningStateLast = createStandardTuning();

    midiCCMapper = std::make_unique<MidiCCHandler>(parameters);

    currentNote = -1;
    engineType = -1;
    
    vuSignal = 0;
    monoMode = 0;

    if (setupMode == SetupMode::userData)
        resolvAppDir();
    
    sendSysexChange = setupMode == SetupMode::userData;
    normalizeDxVelocity = false;
    sysexComm.listener = this;
    sysexComm.setCommandFifo(&command);
    sysexComm.setAVProcessor(this);
    for (size_t index = 0; index < AVSysExQueueSize; ++index)
        avSysExPackets[index].sequence.store(index, std::memory_order_relaxed);
    for (auto& batch : avMidiOutputBatches)
        batch.buffer.ensureSize(AVMidiOutputBatchBytes);
    showKeyboard = true;
    
    memset(&voiceStatus, 0, sizeof(VoiceStatus));
    setEngineType(DEXED_ENGINE_MARKI);
    
    controllers.pitch_range_up = 3;
    controllers.pitch_range_dn = 3;
    controllers.pitch_step = 0;
    controllers.masterTune = 0;
    // AV Dexed: the glissando switch follows its parameter (off by default);
    // Dexed left it unset until the parameter changed, so a new instance
    // could glide in semitones on its own.
    controllers.portamento_gliss_cc = false;
    
    if (setupMode == SetupMode::userData)
        loadPreference();

    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        voices[note].dx7_note = NULL;
    }

    if (setupMode == SetupMode::userData) {
        setupStartupCart();
        setCurrentProgram(0);
    } else {
        currentProgram = 0;
        activeProgram = Program::initialProgram();
        activeProgram.pushToParameters(parameters);
    }

    mtsClient = setupMode == SetupMode::userData ? MTS_RegisterClient() : nullptr;
    captureAVPresetBaseline();
    startTimerHz(30);
}

DexedAudioProcessor::~DexedAudioProcessor() {
    stopTimer();
    releaseResources();
    TRACE("Bye");
    if (mtsClient) MTS_DeregisterClient(mtsClient);
}

//==============================================================================
void DexedAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    avOutputPeakL.store(0.0f, std::memory_order_relaxed);
    avOutputPeakR.store(0.0f, std::memory_order_relaxed);
    Freqlut::init(sampleRate);
    Lfo::init(sampleRate);
    PitchEnv::init(sampleRate);
    Env::init_sr(sampleRate);
    Porta::init_sr(sampleRate);
    fx.init(sampleRate);
    stereoFxBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, true, true);
    effects.prepare(sampleRate, samplesPerBlock);

    masterVolumeSmoother.reset(sampleRate, 0.02);
    panSmoother.reset(sampleRate, 0.02);
    masterVolumeSmoother.setCurrentAndTargetValue(masterVolumeValue.load(std::memory_order_relaxed));
    panSmoother.setCurrentAndTargetValue(panValue.load(std::memory_order_relaxed));

    vuDecayFactor = VuMeterOutput::getDecayFactor(sampleRate);
    
    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        delete voices[note].dx7_note;
        voices[note].dx7_note = new Dx7Note(synthTuningState, mtsClient);
        voices[note].midi_note = -1;
        voices[note].keydown = false;
        voices[note].sustained = false;
        voices[note].live = false;
        voices[note].keydown_seq = -1;
    }

    currentNote = 0;
    nextKeydownSeq = 0;
    controllers.pitch_cc = 0x2000;
    controllers.modwheel_cc = 0;
    avKeyboardPitch.store(8192, std::memory_order_relaxed);
    avKeyboardMod.store(0, std::memory_order_relaxed);
    controllers.foot_cc = 0;
    controllers.breath_cc = 0;
    controllers.aftertouch_cc = 0;
    controllers.portamento_enable_cc = false;
    controllers.portamento_cc = 0;
	controllers.refresh();

    sustain = false;
    monoMode = parameters.getParameter(IDs::monoMode.name)->getValue() > 0.5f;
    avMonoResetRequested.store(false, std::memory_order_relaxed);
    extra_buf_size = 0;

    keyboardState.reset();
    
    lfo.reset(activeProgram.getParameters(137));

    command.reset();
}

void DexedAudioProcessor::releaseResources() {
    currentNote = -1;

    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        if ( voices[note].dx7_note != NULL ) {
            delete voices[note].dx7_note;
            voices[note].dx7_note = NULL;
        }
        voices[note].keydown = false;
        voices[note].sustained = false;
        voices[note].live = false;
    }

    keyboardState.reset();
}

void DexedAudioProcessor::processBlock(AudioSampleBuffer& buffer, MidiBuffer& midiMessages) {
    const auto avCallbackStart = juce::Time::getHighResolutionTicks();
    juce::ScopedNoDenormals noDenormals;

    int numSamples = buffer.getNumSamples();
    int i;

    command.call(*this);
    if (avPanicRequested.exchange(false, std::memory_order_relaxed))
        panic();
    if (avMonoResetRequested.exchange(false, std::memory_order_relaxed))
        resetMonoMode();

    if (numSamples <= 0) {
        finishAVAudioMeter(avCallbackStart, numSamples);
        return;
    }

    if (fxResetRequested.exchange(false, std::memory_order_relaxed))
        effects.reset();

    const int pitch = avKeyboardPitchPending.exchange(-1, std::memory_order_relaxed);
    const int modulation = avKeyboardModPending.exchange(-1, std::memory_order_relaxed);
    if (pitch >= 0) {
        const auto message = juce::MidiMessage::pitchWheel(1, pitch);
        processMidiMessage(&message);
    }
    if (modulation >= 0) {
        const auto message = juce::MidiMessage::controllerEvent(1, 1, modulation);
        processMidiMessage(&message);
    }

    if ( refreshVoice.compareAndSetBool(false, true) ) {
        for(i=0;i < MAX_ACTIVE_NOTES;i++) {
            if ( voices[i].live )
                voices[i].dx7_note->update(activeProgram.getRawData(), voices[i].midi_note, voices[i].velocity, voices[i].channel);
        }
        lfo.reset(activeProgram.getParameters(137));
    }

    keyboardState.processNextMidiBuffer(midiMessages, 0, numSamples, true);

    juce::MidiBufferIterator it = midiMessages.begin(), end = midiMessages.end();
    float *channelData = buffer.getWritePointer(0);
  
    // flush first events
    for (i=0; i < numSamples && i < extra_buf_size; i++) {
        channelData[i] = extra_buf[i];
    }
    
    // remaining buffer is still to be processed
    if (extra_buf_size > numSamples) {
        for (int j = 0; j < extra_buf_size - numSamples; j++) {
            extra_buf[j] = extra_buf[j + numSamples];
        }
        extra_buf_size -= numSamples;
        
        // flush the events, they will be process in the next cycle
        processMidiMessages(it, end, numSamples);
    } else {
        for (; i < numSamples; i += N) {
            AlignedBuf<int32_t, N> audiobuf;
            float sumbuf[N];

            processMidiMessages(it, end, i);

            for (int j = 0; j < N; ++j) {
                audiobuf.get()[j] = 0;
                sumbuf[j] = 0;
            }
            int32_t lfovalue = lfo.getsample();
            int32_t lfodelay = lfo.getdelay();
            
            bool checkMTSESPRetuning = synthTuningState->is_standard_tuning() &&
                                        MTS_HasMaster(mtsClient);
            
            for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
                if (voices[note].live) {
                    
                    if (checkMTSESPRetuning)
                        voices[note].dx7_note->updateBasePitches();
                    
                    voices[note].dx7_note->compute(audiobuf.get(), lfovalue, lfodelay, &controllers);
                    
                    for (int j=0; j < N; ++j) {
                        int32_t val = audiobuf.get()[j];
                        
                        val = val >> 4;
                        int clip_val = val < -(1 << 24) ? 0x8000 : val >= (1 << 24) ? 0x7fff : val >> 9;
                        float f = ((float) clip_val) / (float) 0x8000;
                        if( f > 1 ) f = 1;
                        if( f < -1 ) f = -1;
                        sumbuf[j] += f;
                        audiobuf.get()[j] = 0;
                    }
                }
            }
            
            int jmax = numSamples - i;
            for (int j = 0; j < N; ++j) {
                if (j < jmax) {
                    channelData[i + j] = sumbuf[j];
                } else {
                    extra_buf[j - jmax] = sumbuf[j];
                }
            }
        }
        extra_buf_size = i - numSamples;
    }

    processMidiMessages(it, end, numSamples);
    fx.process(channelData, numSamples);

    // The original synth/filter remain mono. Build a preallocated stereo bus
    // for the optional post-filter effects, then return to the host's layout.
    const double hostBpm = [&]() {
        if (auto* playHead = getPlayHead()) {
            if (auto position = playHead->getPosition()) {
                if (auto bpm = position->getBpm())
                    return *bpm;
            }
        }
        return static_cast<double>(avDelayTempoValue.load(std::memory_order_relaxed));
    }();

    masterVolumeSmoother.setTargetValue(masterVolumeValue.load(std::memory_order_relaxed));
    panSmoother.setTargetValue(panValue.load(std::memory_order_relaxed));
    float* outputLeft = buffer.getWritePointer(0);
    float* outputRight = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
    float avBlockPeakL = 0.0f, avBlockPeakR = 0.0f;
    int processed = 0;
    while (processed < numSamples) {
        const int chunk = juce::jmin(numSamples - processed, stereoFxBuffer.getNumSamples());
        auto* fxLeft = stereoFxBuffer.getWritePointer(0);
        auto* fxRight = stereoFxBuffer.getWritePointer(1);
        for (int sample = 0; sample < chunk; ++sample) {
            const float mono = channelData[processed + sample];
            fxLeft[sample] = mono;
            fxRight[sample] = mono;
        }
        effects.process(stereoFxBuffer, chunk, hostBpm);

        const auto* processedLeft = stereoFxBuffer.getReadPointer(0);
        const auto* processedRight = stereoFxBuffer.getReadPointer(1);
        for (int sample = 0; sample < chunk; ++sample) {
            const float master = masterVolumeSmoother.getNextValue();
            const float pan = panSmoother.getNextValue();
            const float leftBalance = pan <= 0.5f ? 1.0f : 2.0f * (1.0f - pan);
            const float rightBalance = pan >= 0.5f ? 1.0f : 2.0f * pan;
            const float left = processedLeft[sample] * master * leftBalance;
            const float right = processedRight[sample] * master * rightBalance;
            outputLeft[processed + sample] = outputRight != nullptr ? left : (left + right) * 0.5f;
            if (outputRight != nullptr)
                outputRight[processed + sample] = right;
            avBlockPeakL = juce::jmax(avBlockPeakL, std::abs(outputLeft[processed + sample]));
            avBlockPeakR = juce::jmax(avBlockPeakR, std::abs(outputRight != nullptr ? right : outputLeft[processed + sample]));

            const float meterSample = juce::jmax(std::abs(left), std::abs(right));
            if (meterSample > vuSignal)
                vuSignal = meterSample;
            else if (vuSignal > 1.26E-4F)
                vuSignal *= vuDecayFactor;
            else
                vuSignal = 0;
        }
        processed += chunk;
    }

    // Max until consumed by the editor: short transients between UI frames
    // remain visible. No memory allocation or message-thread synchronization.
    const auto publishPeak = [](std::atomic<float>& destination, float peak) {
        auto previous = destination.load(std::memory_order_relaxed);
        while (previous < peak && !destination.compare_exchange_weak(previous, peak, std::memory_order_relaxed)) {}
    };
    publishPeak(avOutputPeakL, avBlockPeakL);
    publishPeak(avOutputPeakR, avBlockPeakR);

    midiMessages.clear();
    auto& avOutput = avMidiOutputBatches[static_cast<size_t>(avMidiOutputRead)];
    if (avOutput.state.load(std::memory_order_acquire) == 1) {
        midiMessages.swapWith(avOutput.buffer);
        avOutput.state.store(2, std::memory_order_release);
        avMidiOutputRead = (avMidiOutputRead + 1) % AVMidiOutputQueueSize;
    }
    finishAVAudioMeter(avCallbackStart, numSamples);
}

void DexedAudioProcessor::finishAVAudioMeter(juce::int64 startTicks, int samples) noexcept {
    int playing = 0;
    for (const auto& voice : voices)
        if (voice.dx7_note != nullptr && voice.live && voice.dx7_note->isPlaying()) ++playing;
    avActiveVoices.store(playing, std::memory_order_relaxed);
    if (samples <= 0 || getSampleRate() <= 0.0) return;
    const auto elapsed = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - startTicks);
    const float percent = static_cast<float>(100.0 * elapsed * getSampleRate() / samples);
    const auto previous = avAudioCpuPercent.load(std::memory_order_relaxed);
    avAudioCpuPercent.store(previous * 0.85f + percent * 0.15f, std::memory_order_relaxed);
}

bool DexedAudioProcessor::queueAVMidiOutputBatch(const juce::MidiBuffer& messages) {
    auto& batch = avMidiOutputBatches[static_cast<size_t>(avMidiOutputWrite)];
    const int state = batch.state.load(std::memory_order_acquire);
    if (state == 1) {
        avDroppedSysEx.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    batch.buffer.clear();
    batch.buffer.ensureSize(AVMidiOutputBatchBytes); // recycled host storage, UI thread only
    batch.buffer.addEvents(messages, 0, -1, 0);
    batch.state.store(1, std::memory_order_release);
    avMidiOutputWrite = (avMidiOutputWrite + 1) % AVMidiOutputQueueSize;
    return true;
}

//==============================================================================
// This creates new instances of the plugin..
AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new DexedAudioProcessor();
}

#define ACT(v) (v.keydown ? v.midi_note : -1)

void DexedAudioProcessor::keydown(uint8_t channel, uint8_t pitch, uint8_t velo) {
    if ( velo == 0 ) {
        keyup(channel, pitch, velo);
        return;
    }

    // In int: a voice transposed beyond its range (a bank value above 48, as
    // Dexed 1.1.0 loads it) must not wrap the note or read past the 128-note
    // tuning table; it sounds at the nearest note there is.
    pitch = static_cast<uint8_t>(juce::jlimit(0, 127, int(pitch) + tuningTranspositionShift()));

    if ( normalizeDxVelocity ) {
        velo = ((float)velo) * 0.7874015; // 100/127
    }

    // Single fused pass: voice-stealing scoring + triggerLfo (any keydown?)
    // + MPE same-channel-keydown detection.
    // Stealing preference (highest score wins; ties broken by least-recent keydown):
    //   +4 not playing, +2 key up, +1 same pitch.
    bool triggerLfo = true;
    int bestNote   = currentNote;
    int bestScore  = -1;
    int note       = currentNote;
    for (int i = 0; i < MAX_ACTIVE_NOTES; i++) {
        const ProcessorVoice &v = voices[note];

        if ( v.keydown ) {
            triggerLfo = false;
            // Two keydowns on the same channel => source is not MPE.
            if ( controllers.mpeEnabled && v.channel == channel )
                controllers.mpeEnabled = false;
        }

        int score = 0;
        if ( !v.dx7_note->isPlaying() ) score += 4;
        if ( !v.keydown )               score += 2;
        if (  v.midi_note == pitch )    score += 1;
        if ( score > bestScore ||
             (score == bestScore && v.keydown_seq < voices[bestNote].keydown_seq) ) {
            bestNote  = note;
            bestScore = score;
        }

        note = (note + 1) % MAX_ACTIVE_NOTES;
    }

    if ( triggerLfo )
        lfo.keydown();

    note = bestNote;

    currentNote = (note + 1) % MAX_ACTIVE_NOTES;
    voices[note].channel = channel;
    voices[note].midi_note = pitch;
    voices[note].velocity = velo;
    voices[note].sustained = sustain;
    voices[note].keydown = true;
    voices[note].keydown_seq = nextKeydownSeq++;
    // to avoid click, don't sync oscillators when voice stealing
    bool voice_steal = voices[note].dx7_note->isPlaying();
    voices[note].dx7_note->init(activeProgram.getRawData(), pitch, velo, channel, &controllers);
    if ( activeProgram[IDs::oscKeySync.pos] && !voice_steal ) {
        voices[note].dx7_note->oscSync();
    }
    if ( (voices[lastActiveVoice].midi_note != -1 && controllers.portamento_enable_cc)
       && controllers.portamento_cc > 0 ) {
        voices[note].dx7_note->initPortamento(*voices[lastActiveVoice].dx7_note);
    }

    if ( monoMode ) {
        for(int i=0; i<MAX_ACTIVE_NOTES; i++) {            
            if ( voices[i].live ) {
                // all keys are up, only transfer signal
                if ( ! voices[i].keydown ) {
                    voices[i].live = false;
                    voices[note].dx7_note->transferSignal(*voices[i].dx7_note);
                    break;
                }
                if ( voices[i].midi_note < pitch ) {
                    voices[i].live = false;
                    voices[note].dx7_note->transferState(*voices[i].dx7_note);
                    break;
                }
                return;
            }
        }
    }
    else if ( !activeProgram[IDs::oscKeySync.pos] ) {
        // if another note at the same pitch is playing, transfer phase
        // to avoid unpredictable destructive interference. this can cause
        // clicking when voice stealing, but we've tried to choose a voice
        // to steal that will minimise the chances of clicking
        for(int i=0; i<MAX_ACTIVE_NOTES; i++) {
            if ( i != note && voices[i].dx7_note->isPlaying() && voices[i].midi_note == pitch ) {
                voices[note].dx7_note->transferPhase(*voices[i].dx7_note);
                break;
            }
        }
    }

 
    voices[note].live = true;
    lastActiveVoice = note;
	//TRACE("activate %d [ %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d ]", pitch, ACT(voices[0]), ACT(voices[1]), ACT(voices[2]), ACT(voices[3]), ACT(voices[4]), ACT(voices[5]), ACT(voices[6]), ACT(voices[7]), ACT(voices[8]), ACT(voices[9]), ACT(voices[10]), ACT(voices[11]), ACT(voices[12]), ACT(voices[13]), ACT(voices[14]), ACT(voices[15]));
}

void DexedAudioProcessor::keyup(uint8_t chan, uint8_t pitch, uint8_t velo) {
    pitch = static_cast<uint8_t>(juce::jlimit(0, 127, int(pitch) + tuningTranspositionShift()));   // as keydown

    int note;
    for (note=0; note<MAX_ACTIVE_NOTES; ++note) {
        if ( ( ( controllers.mpeEnabled && voices[note].channel == chan ) || // MPE node - find voice by channel
               (!controllers.mpeEnabled && voices[note].midi_note == pitch ) ) && // regular mode find voice by pitch
            voices[note].keydown ) { // but still only grab the one which is keydown
            voices[note].keydown = false;
			//TRACE("deactivate %d [ %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d ]", pitch, ACT(voices[0]), ACT(voices[1]), ACT(voices[2]), ACT(voices[3]), ACT(voices[4]), ACT(voices[5]), ACT(voices[6]), ACT(voices[7]), ACT(voices[8]), ACT(voices[9]), ACT(voices[10]), ACT(voices[11]), ACT(voices[12]), ACT(voices[13]), ACT(voices[14]), ACT(voices[15]));
            break;
        }
    }
    
    // note not found ?
    if ( note >= MAX_ACTIVE_NOTES ) {
        return;
    }
    
    if ( monoMode ) {
        int highNote = -1;
        int target = 0;
        for (int i=0; i<MAX_ACTIVE_NOTES;i++) {
            if ( voices[i].keydown && voices[i].midi_note > highNote ) {
                target = i;
                highNote = voices[i].midi_note;
            }
        }
        
        if ( highNote != -1 && voices[note].live ) {
            voices[note].live = false;
            voices[target].live = true;
            voices[target].dx7_note->transferState(*voices[note].dx7_note);
        }
    }
    
    if ( sustain ) {
        voices[note].sustained = true;
    } else {
        voices[note].dx7_note->keyup();
    }
}

void DexedAudioProcessor::panic() {
    fxResetRequested.store(true, std::memory_order_relaxed);
    extra_buf_size = 0;
    sustain = false;
    for(int i=0;i<MAX_ACTIVE_NOTES;i++) {
        voices[i].midi_note = -1;
        voices[i].keydown = false;
        voices[i].sustained = false;
        voices[i].live = false;
        if ( voices[i].dx7_note != NULL ) {
            voices[i].dx7_note->oscSync();
        }
    }
    keyboardState.reset();
}

int DexedAudioProcessor::getEngineType() {
    return engineType;
}

void DexedAudioProcessor::setEngineType(int tp) {

    switch (tp)  {
        case DEXED_ENGINE_MARKI:
            controllers.core = &engineMkI;
            break;
        case DEXED_ENGINE_OPL:
            controllers.core = &engineOpl;
            break;
        default:
            controllers.core = &engineMsfa;
            break;
    }
    engineType = tp;
}

void DexedAudioProcessor::resetMonoMode() {
    panic();
    monoMode = parameters.getParameter(IDs::monoMode.name)->getValue() > 0.5f;
}

// ====================================================================
bool DexedAudioProcessor::peekVoiceStatus() {
    if ( currentNote == -1 )
        return false;

    // Report what is actually sounding: each operator's loudest level across
    // every voice whose carriers can still be heard, and envelope steps from
    // the newest of those voices. A voice whose carriers have decayed below the
    // floor of the Play glow (2^18, the bottom of its 48 dB window) is silent
    // even while its key is held, so its sustaining modulators stay dark.
    constexpr uint32_t audibleCarrierFloor = 1u << 18;
    VoiceStatus combined {}, one {};
    int32_t newest = 0;
    bool any = false;
    for (auto& voice : voices) {
        if (voice.dx7_note == nullptr || !voice.live || !voice.dx7_note->isPlaying()
            || !voice.dx7_note->hasAudibleCarrier(audibleCarrierFloor))
            continue;
        voice.dx7_note->peekVoiceStatus(one);
        for (int op = 0; op < 6; ++op)
            combined.amp[op] = std::max(combined.amp[op], one.amp[op]);
        if (!any || voice.keydown_seq > newest) {
            newest = voice.keydown_seq;
            std::memcpy(combined.ampStep, one.ampStep, sizeof(combined.ampStep));
            combined.pitchStep = one.pitchStep;
        }
        any = true;
    }
    if (!any) {
        std::memset(voiceStatus.amp, 0, sizeof(voiceStatus.amp));
        return false;
    }
    voiceStatus = combined;
    return true;
}

bool DexedAudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const {
    return layouts.getMainOutputChannelSet() == AudioChannelSet::mono()
                || layouts.getMainOutputChannelSet() == AudioChannelSet::stereo();
}

bool DexedAudioProcessor::acceptsMidi() const {
    return true;
}

bool DexedAudioProcessor::producesMidi() const {
    return true;
}

double DexedAudioProcessor::getTailLengthSeconds() const {
    if (effects.hasDelayTail()) return 50.0;
    if (effects.hasReverbTail()) return 18.0;
    return 0.0;
}

const String DexedAudioProcessor::getName() const {
    return JucePlugin_Name;
}

//==============================================================================
bool DexedAudioProcessor::hasEditor() const {
    return true; // (change this to false if you choose to not supply an editor)
}

AudioProcessorEditor* DexedAudioProcessor::createEditor() {
    AudioProcessorEditor* editor = new DexedAudioProcessorEditor (this);
    return editor;
}

void DexedAudioProcessor::setZoomFactor(float factor) {
    zoomFactor = factor;
}

void DexedAudioProcessor::applyProgram(const Program &program) {
    // Init, a received voice...: setCurrentProgram marks a bank slot's sound again.
    avSoundFromBankSlot.store(false, std::memory_order_relaxed);
    if (midiCCMapper != nullptr) midiCCMapper->forgetAVCCWrites();   // a new sound: the wheel moved nothing of it
    panic();
    activeProgram = program;
    activeProgram.pushToParameters(parameters);
    lfo.reset(activeProgram.getParameters(IDs::lfoRate.pos));
    refreshVoice.set(true);
    programChanged.set(true);
    avPresetContextCurrent.store(false, std::memory_order_relaxed);
    avKnownPresetOrigin.store(false, std::memory_order_relaxed);
    captureAVPresetBaseline();
}

juce::String DexedAudioProcessor::getAVPresetName() const {
    return avPresetContextCurrent.load(std::memory_order_relaxed) && avPresetName.isNotEmpty()
        ? avPresetName : activeProgram.getName().trimEnd();
}

juce::String DexedAudioProcessor::getAVPresetId() const {
    return avPresetContextCurrent.load(std::memory_order_relaxed) ? avPresetId : juce::String();
}

juce::File DexedAudioProcessor::getAVPresetPath() const {
    return avPresetContextCurrent.load(std::memory_order_relaxed) ? avPresetPath : juce::File();
}

void DexedAudioProcessor::setAVPresetContext(const juce::String& name, const juce::String& id,
                                            const juce::File& file) {
    avPresetName = name;
    avPresetId = id;
    avPresetPath = file;
    const bool hasContext = name.isNotEmpty() || id.isNotEmpty() || file.getFullPathName().isNotEmpty();
    avPresetContextCurrent.store(hasContext, std::memory_order_relaxed);
    if (hasContext)
        avKnownPresetOrigin.store(true, std::memory_order_relaxed);
    programChanged.set(true);
}

uint64_t DexedAudioProcessor::getAVMusicalFingerprint(const juce::String* nativeName) const noexcept {
    // FNV-1a over musical content only. No ValueTree cloning, disk access, or
    // allocation: program changes arriving through MIDI can capture this too.
    uint64_t hash = 14695981039346656037ULL;
    const auto append = [&hash](const void* bytes, size_t count) {
        const auto* data = static_cast<const uint8_t*>(bytes);
        for (size_t i = 0; i < count; ++i)
            hash = (hash ^ data[i]) * 1099511628211ULL;
    };
    // A wheel gesture is played, not part of a sound: a parameter a mapped CC
    // moved (CC1 → Cutoff by default) counts with the value the sound had, so
    // playing the wheel never marks the sound "Modificado"; moving the same
    // knob with the mouse does. Atomics only, no allocation.
    for (const auto* parameter : getParameters()) {
        // "Volumen general" and the local tempo belong to the instrument, not the sound.
        if (parameter == avOutputParameter || parameter == avDelayTempoParameter)
            continue;
        const float soundValue = midiCCMapper != nullptr ? midiCCMapper->getAVSoundValue(parameter) : parameter->getValue();
        const auto value = static_cast<int32_t>(std::lround(soundValue * 1000000.0f));
        append(&value, sizeof(value));
    }
    append(activeProgram.getRawData(), Program::PROGRAM_SIZE + 1);
    append(currentSCLData.data(), currentSCLData.size());
    append(currentKBMData.data(), currentKBMData.size());
    if (midiCCMapper != nullptr) {
        const auto mappings = midiCCMapper->getAVMappingsFingerprint();
        append(&mappings, sizeof(mappings));
    }
    if (nativeName != nullptr)
        append(nativeName->toRawUTF8(), static_cast<size_t>(nativeName->getNumBytesAsUTF8()));
    else if (avPresetContextCurrent.load(std::memory_order_relaxed))
        append(avPresetName.toRawUTF8(), static_cast<size_t>(avPresetName.getNumBytesAsUTF8()));
    return hash;
}

void DexedAudioProcessor::captureAVPresetBaseline() noexcept {
    avPresetBaseline.store(getAVMusicalFingerprint(), std::memory_order_relaxed);
}

bool DexedAudioProcessor::isAVPresetModified() const noexcept {
    const auto baseline = avPresetBaseline.load(std::memory_order_relaxed);
    return baseline != 0 && baseline != getAVMusicalFingerprint();
}

void dexed_trace(const char *source, const char *fmt, ...) {
    char output[4096];
    va_list argptr;
    va_start(argptr, fmt);
    vsnprintf(output, 4095, fmt, argptr);
    va_end(argptr);

    String dest;
    dest << source << " " << output;
    Logger::writeToLog(dest);
}
