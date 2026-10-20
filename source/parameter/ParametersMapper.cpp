#include "../midi/MidiCCHandler.h"
#include "Model.h"
#include "PluginProcessor.h"
#include "core/Program.h"
#include <vector>

#define APPLY_OP(i)              \
    IDs::egRate.op(i).idx(0),    \
    IDs::egRate.op(i).idx(1),    \
    IDs::egRate.op(i).idx(2),    \
    IDs::egRate.op(i).idx(3),    \
    IDs::egLevel.op(i).idx(0),   \
    IDs::egLevel.op(i).idx(1),   \
    IDs::egLevel.op(i).idx(2),   \
    IDs::egLevel.op(i).idx(3),   \
    IDs::breakpoint.op(i),       \
    IDs::lScaleDepth.op(i),      \
    IDs::rScaleDepth.op(i),      \
    IDs::lKeyScale.op(i),        \
    IDs::rKeyScale.op(i),        \
    IDs::rateScaling.op(i),      \
    IDs::ampModeSens.op(i),      \
    IDs::keyVelocity.op(i),      \
    IDs::outputLevel.op(i),      \
    IDs::mode.op(i),             \
    IDs::frequencyCoarse.op(i),  \
    IDs::frequencyFine.op(i),    \
    IDs::detune.op(i)

const std::vector<MetaParameterID> dxParameters = {
    APPLY_OP(0),
    APPLY_OP(1),
    APPLY_OP(2),
    APPLY_OP(3),
    APPLY_OP(4),
    APPLY_OP(5),
    IDs::pitchEgRate.idx(0),            // pos: 126
    IDs::pitchEgRate.idx(1),
    IDs::pitchEgRate.idx(2),
    IDs::pitchEgRate.idx(3),
    IDs::pitchEgLevel.idx(0),           // pos: 130
    IDs::pitchEgLevel.idx(1),
    IDs::pitchEgLevel.idx(2),
    IDs::pitchEgLevel.idx(3),
    IDs::algorithm,                  // pos: 134
    IDs::feedback,                   // pos: 135
    IDs::oscKeySync,                 // pos: 136
    IDs::lfoRate,                    // pos: 137
    IDs::lfoDelay,                   // pos: 138
    IDs::lfoPmDepth,                 // pos: 139
    IDs::lfoAmpDepth,                // pos: 140
    IDs::lfoKeySync,                 // pos: 141
    IDs::lfoWaveform,                // pos: 142
    IDs::pitchModSens,               // pos: 143
    IDs::transpose                   // pos: 144
};

void DexedAudioProcessor::mapParameters() {
    // We set the 154 parameters
    for (const auto &param : dxParameters) {
        parameters.mapTo(param.name, [this, param](float newValue) {
            if ( parameters.pushToParameterInProgress )
                return;
            uint8_t value = static_cast<uint8_t>(static_cast<int>(newValue) + param.displayOffset);
            setDxValue(param.pos, value);
        });
    }

    // Handle operator ON switches
    for (int i=0; i<6; i++) {
        parameters.mapTo(IDs::on.op(i).name, [this, i](float newValue) {
            if ( newValue > 0.5f ) {
                activeProgram[IDs::on.pos] |= (1U << (5-i));
            } else {
                activeProgram[IDs::on.pos] &= ~(1U << (5-i));
            }
            refreshVoice.set(true);
        });
    }

    parameters.mapTo(IDs::cutoff.name, [this](float newValue) {
        fx.uiCutoff = newValue;
    });

    parameters.mapTo(IDs::resonance.name, [this](float newValue) {
        fx.uiReso = newValue;
    });

    parameters.mapTo(IDs::output.name, [this](float newValue) {
        fx.uiGain = newValue;
    });

    parameters.mapTo(IDs::masterTuneAdj.name, [this](float newValue) {
        int32_t tune = ( (newValue/2+0.5) * 0x4000) - 0x2000;
        controllers.masterTune = ((float) (tune << 11)) * (1.0/12);
    });

    parameters.mapTo(IDs::monoMode.name, [this](float newValue) {
        avMonoResetRequested.store(true, std::memory_order_relaxed);
    });

    // Modulation parameters - Wheel
    parameters.mapTo(IDs::modWheel.name, [this](float newValue) {
        controllers.wheel.range = static_cast<int>(newValue);
        controllers.refresh();
    });
    parameters.mapTo(IDs::modWheelPitch.name, [this](float newValue) {
        controllers.wheel.pitch = newValue > 0.5f;
        controllers.refresh();
    });
    parameters.mapTo(IDs::modWheelAmp.name, [this](float newValue) {
        controllers.wheel.amp = newValue > 0.5f;
        controllers.refresh();
    });
    parameters.mapTo(IDs::modWheelEgBias.name, [this](float newValue) {
        controllers.wheel.eg = newValue > 0.5f;
        controllers.refresh();
    });

    // Modulation parameters - Foot
    parameters.mapTo(IDs::modFoot.name, [this](float newValue) {
        controllers.foot.range = static_cast<int>(newValue);
        controllers.refresh();
    });
    parameters.mapTo(IDs::modFootPitch.name, [this](float newValue) {
        controllers.foot.pitch = newValue > 0.5f;
        controllers.refresh();
    });
    parameters.mapTo(IDs::modFootAmp.name, [this](float newValue) {
        controllers.foot.amp = newValue > 0.5f;
        controllers.refresh();
    });
    parameters.mapTo(IDs::modFootEgBias.name, [this](float newValue) {
        controllers.foot.eg = newValue > 0.5f;
        controllers.refresh();
    });

    // Modulation parameters - Breath
    parameters.mapTo(IDs::modBreath.name, [this](float newValue) {
        controllers.breath.range = static_cast<int>(newValue);
        controllers.refresh();
    });
    parameters.mapTo(IDs::modBreathPitch.name, [this](float newValue) {
        controllers.breath.pitch = newValue > 0.5f;
        controllers.refresh();
    });
    parameters.mapTo(IDs::modBreathAmp.name, [this](float newValue) {
        controllers.breath.amp = newValue > 0.5f;
        controllers.refresh();
    });
    parameters.mapTo(IDs::modBreathEgBias.name, [this](float newValue) {
        controllers.breath.eg = newValue > 0.5f;
        controllers.refresh();
    });

    // Modulation parameters - Aftertouch
    parameters.mapTo(IDs::modAftertouch.name, [this](float newValue) {
        controllers.at.range = static_cast<int>(newValue);
        controllers.refresh();
    });
    parameters.mapTo(IDs::modAftertouchPitch.name, [this](float newValue) {
        controllers.at.pitch = newValue > 0.5f;
        controllers.refresh();
    });
    parameters.mapTo(IDs::modAftertouchAmp.name, [this](float newValue) {
        controllers.at.amp = newValue > 0.5f;
        controllers.refresh();
    });
    parameters.mapTo(IDs::modAftertouchEgBias.name, [this](float newValue) {
        controllers.at.eg = newValue > 0.5f;
        controllers.refresh();
    });

    parameters.mapTo(IDs::transposeTuningScale.name, [this](float newValue) {
        controllers.transpose12AsScale = newValue > 0.5f;
    });

    parameters.mapTo(IDs::glissando.name, [this](float newValue) {
        controllers.portamento_gliss_cc = newValue > 0.5f;
    });

    parameters.mapTo(IDs::portamentoTm.name, [this](float newValue) {
        controllers.portamento_cc = static_cast<int32_t>(newValue * 127.0f / 99.0f);
        controllers.portamento_enable_cc = controllers.portamento_cc > 0;
    });

    parameters.mapTo(IDs::engineType.name, [this](float newValue) {
        setEngineType(static_cast<int>(std::round(newValue)));
    });

    parameters.mapTo(IDs::pitchRangeUp.name, [this](float newValue) {
        controllers.pitch_range_up = static_cast<int>(newValue);
    });

    parameters.mapTo(IDs::pitchRangeDown.name, [this](float newValue) {
        controllers.pitch_range_dn = static_cast<int>(newValue);
    });

    parameters.mapTo(IDs::pitchStep.name, [this](float newValue) {
        controllers.pitch_step = static_cast<int>(newValue);
    });

    parameters.mapTo(IDs::masterVolume.name, [this](float newValue) {
        masterVolumeValue.store(juce::jlimit(0.0f, AVMasterMaximumGain, newValue), std::memory_order_relaxed);
    });

    parameters.mapTo(IDs::pan.name, [this](float newValue) {
        panValue.store(juce::jlimit(0.0f, 1.0f, newValue), std::memory_order_relaxed);
    });
    parameters.mapTo(IDs::avDelayTempo.name, [this](float value) {
        avDelayTempoValue.store(juce::jlimit(20.0f, 999.0f, value), std::memory_order_relaxed);
    });
    parameters.mapTo(IDs::avReverbBypass.name, [this](float value) { effects.setReverbBypass(value > 0.5f); });
    parameters.mapTo(IDs::avDelayBypass.name, [this](float value) { effects.setDelayBypass(value > 0.5f); });
    parameters.mapTo(IDs::avChorusBypass.name, [this](float value) { effects.setChorusBypass(value > 0.5f); });

    parameters.mapTo(IDs::reverbAmount.name, [this](float value) { effects.setReverbAmount(value); });
    parameters.mapTo(IDs::reverbSize.name, [this](float value) { effects.setReverbSize(value); });
    parameters.mapTo(IDs::reverbPreDelay.name, [this](float value) {
        effects.setReverbPreDelay(value / AVDexedReverb::maxPredelayMs);
    });
    parameters.mapTo(IDs::reverbDamping.name, [this](float value) { effects.setReverbDamping(value); });
    parameters.mapTo(IDs::delayAmount.name, [this](float value) { effects.setDelayAmount(value); });
    parameters.mapTo(IDs::delaySync.name, [this](float value) { effects.setDelaySync(value > 0.5f); });
    parameters.mapTo(IDs::delayDivisionLeft.name, [this](float value) { effects.setDelayDivisionLeft(static_cast<int>(std::lround(value))); });
    parameters.mapTo(IDs::delayDivisionRight.name, [this](float value) { effects.setDelayDivisionRight(static_cast<int>(std::lround(value))); });
    parameters.mapTo(IDs::delayTimeLeft.name, [this](float value) { effects.setDelayTimeLeft(value); });
    parameters.mapTo(IDs::delayTimeRight.name, [this](float value) { effects.setDelayTimeRight(value); });
    parameters.mapTo(IDs::delayFeedback.name, [this](float value) { effects.setDelayFeedback(value); });
    parameters.mapTo(IDs::delayDamping.name, [this](float value) { effects.setDelayDamping(value); });
    parameters.mapTo(IDs::chorusAmount.name, [this](float value) { effects.setChorusAmount(value); });
    parameters.mapTo(IDs::chorusMode.name, [this](float value) { effects.setChorusMode(static_cast<int>(std::lround(value))); });
    parameters.mapTo(IDs::chorusRate.name, [this](float value) { effects.setChorusRate(value); });
    parameters.mapTo(IDs::chorusDepth.name, [this](float value) { effects.setChorusDepth(value); });
    parameters.mapTo(IDs::chorusSpread.name, [this](float value) { effects.setChorusSpread(value); });

    const auto valueFor = [this](const MetaParameterID& id) {
        auto* parameter = parameters.getParameter(id.name);
        return parameter != nullptr ? parameter->convertFrom0to1(parameter->getValue()) : 0.0f;
    };
    masterVolumeValue.store(valueFor(IDs::masterVolume), std::memory_order_relaxed);
    panValue.store(valueFor(IDs::pan), std::memory_order_relaxed);
    effects.setReverbAmount(valueFor(IDs::reverbAmount));
    effects.setReverbSize(valueFor(IDs::reverbSize));
    effects.setReverbPreDelay(valueFor(IDs::reverbPreDelay) / AVDexedReverb::maxPredelayMs);
    effects.setReverbDamping(valueFor(IDs::reverbDamping));
    effects.setDelayAmount(valueFor(IDs::delayAmount));
    effects.setDelaySync(valueFor(IDs::delaySync) > 0.5f);
    effects.setDelayDivisionLeft(static_cast<int>(std::lround(valueFor(IDs::delayDivisionLeft))));
    effects.setDelayDivisionRight(static_cast<int>(std::lround(valueFor(IDs::delayDivisionRight))));
    effects.setDelayTimeLeft(valueFor(IDs::delayTimeLeft));
    effects.setDelayTimeRight(valueFor(IDs::delayTimeRight));
    effects.setDelayFeedback(valueFor(IDs::delayFeedback));
    effects.setDelayDamping(valueFor(IDs::delayDamping));
    effects.setChorusAmount(valueFor(IDs::chorusAmount));
    effects.setChorusMode(static_cast<int>(std::lround(valueFor(IDs::chorusMode))));
    effects.setChorusRate(valueFor(IDs::chorusRate));
    effects.setChorusDepth(valueFor(IDs::chorusDepth));
    effects.setChorusSpread(valueFor(IDs::chorusSpread));
}

int DexedAudioProcessor::getNumPrograms() {
    return 32;
}

int DexedAudioProcessor::getCurrentProgram() {
    return currentProgram;
}

// The modern settings that belong to each sound. Not avDelayTempo (the local
// BPM is global, in Settings) nor output (the global volume).
static const MetaParameterID* const avSoundParameterIds[] = {
    &IDs::masterVolume, &IDs::pan, &IDs::cutoff, &IDs::resonance,
    &IDs::reverbAmount, &IDs::reverbSize, &IDs::reverbPreDelay, &IDs::reverbDamping,
    &IDs::delayAmount, &IDs::delaySync, &IDs::delayDivisionLeft, &IDs::delayDivisionRight,
    &IDs::delayTimeLeft, &IDs::delayTimeRight, &IDs::delayFeedback, &IDs::delayDamping,
    &IDs::chorusAmount, &IDs::chorusMode, &IDs::chorusRate, &IDs::chorusDepth, &IDs::chorusSpread,
    &IDs::avReverbBypass, &IDs::avDelayBypass, &IDs::avChorusBypass
};

void DexedAudioProcessor::resolveAVSoundParameters() {
    static_assert(std::size(avSoundParameterIds) == AVSoundParameterCount);
    for (size_t i = 0; i < avSoundParameters.size(); ++i) {
        avSoundParameters[i] = parameters.getParameter(avSoundParameterIds[i]->name);
        jassert(avSoundParameters[i] != nullptr);
    }
    avOutputParameter = parameters.getParameter(IDs::output.name);
    avDelayTempoParameter = parameters.getParameter(IDs::avDelayTempo.name);
    jassert(avOutputParameter != nullptr && avDelayTempoParameter != nullptr);
}

DexedAudioProcessor::AVSoundTable DexedAudioProcessor::makeAVSoundTable(const AVDexedLibrary::BankSettings& settings) const {
    AVSoundTable table {};
    for (const auto& [slot, stored] : settings) {
        if (!juce::isPositiveAndBelow(slot, 32))
            continue;
        auto& entry = table[static_cast<size_t>(slot)];
        entry.voice = stored.voice;
        entry.hasSettings = true;
        for (size_t i = 0; i < avSoundParameters.size(); ++i) {
            const auto* parameter = avSoundParameters[i];
            entry.values[i] = parameter != nullptr ? parameter->getDefaultValue() : 0.0f;
            if (parameter == nullptr)
                continue;
            // Real units, as in session state; ids this version does not know are ignored.
            const auto found = stored.values.find(parameter->paramID);
            if (found != stored.values.end() && std::isfinite(found->second))
                entry.values[i] = juce::jlimit(0.0f, 1.0f, parameter->convertTo0to1(found->second));
        }
    }
    return table;
}

void DexedAudioProcessor::applyAVSoundSettings(int program) {
    // Settings stay with their sound: the slot's own entry when it still holds
    // that voice, else any slot holding the same voice (moved in the bank
    // manager), else the neutral defaults.
    const auto voice = AVDexedLibrary::voiceHash(currentCart, program);
    AVSoundSlot slot;
    {
        const juce::SpinLock::ScopedLockType lock(avSoundTableLock);
        const auto& own = avSoundTable[static_cast<size_t>(program)];
        if (own.hasSettings && own.voice == voice) {
            slot = own;
        } else {
            for (const auto& other : avSoundTable) {
                if (other.hasSettings && other.voice == voice) {
                    slot = other;
                    break;
                }
            }
        }
    }

    // Only what differs, and without change gestures: a sound change is not an
    // edit, so hosts in Touch/Latch do not record it as automation.
    for (size_t i = 0; i < avSoundParameters.size(); ++i) {
        auto* parameter = avSoundParameters[i];
        if (parameter == nullptr)
            continue;
        const float target = slot.hasSettings ? slot.values[i] : parameter->getDefaultValue();
        if (std::abs(parameter->getValue() - target) > 1.0e-6f)
            parameter->setValueNotifyingHost(target);
    }
}

AVDexedLibrary::SlotSettings DexedAudioProcessor::captureAVSoundSettings() const {
    AVDexedLibrary::SlotSettings settings;
    // The voice as it would be stored: the current program packed into its
    // slot. Like "Guardar", an unchanged voice keeps the bank's own bytes.
    Cartridge packed = currentCart;
    const int slot = juce::jlimit(0, 31, currentProgram);
    packAVVoiceIfChanged(packed, slot, activeProgram);
    settings.voice = AVDexedLibrary::voiceHash(packed, slot);

    for (const auto* parameter : avSoundParameters) {
        if (parameter == nullptr)
            continue;
        // The sound's own value, not where the mod wheel left it.
        const auto value = midiCCMapper != nullptr ? midiCCMapper->getAVSoundValue(parameter) : parameter->getValue();
        if (std::abs(value - parameter->getDefaultValue()) > 1.0e-4f)
            settings.values[parameter->paramID] = parameter->convertFrom0to1(value);
    }
    return settings;
}

namespace {
/** The voices of a bank, as the restore guard tells banks apart. No allocation:
    the audio thread computes it on a MIDI program change. */
juce::uint64 avBankVoicesHash(const Cartridge& bank) noexcept {
    return AVDexedLibrary::stableHash64(bank.getVoiceSysex() + 6, 4096);
}
}

void DexedAudioProcessor::armAVRestoreGuard() noexcept {
    avRestoreGuardTicksLeft.store(AVRestoreGuardTicks, std::memory_order_relaxed);
    avRestoreGuardBank.store(avBankVoicesHash(currentCart), std::memory_order_relaxed);
    avRestoreGuardProgram.store(currentProgram, std::memory_order_release);
}

void DexedAudioProcessor::tickAVRestoreGuard() noexcept {
    if (avRestoreGuardTicksLeft.load(std::memory_order_relaxed) > 0
        && avRestoreGuardTicksLeft.fetch_sub(1, std::memory_order_relaxed) == 1)
        avRestoreGuardProgram.store(-1, std::memory_order_release);
}

void DexedAudioProcessor::setCurrentProgramSelection(int index) {
    // Session restore and visible selection: the sound and its settings stay.
    currentProgram = juce::jlimit(0, 31, index);
    programChanged.set(true);
}

void DexedAudioProcessor::setCurrentProgram(int index) {
    index = juce::jlimit(0, 31, index);
    // The first program change after a host restore consumes the guard; when
    // it re-selects the restored program of the restored bank, the session's
    // values win. The same slot of a bank installed since (another bank chosen
    // in the browser, a SysEx bank) is another sound: it gets its own settings.
    // A bank with the same voices only re-pointed to another file keeps it.
    const int restoredProgram = avRestoreGuardProgram.exchange(-1, std::memory_order_acq_rel);
    const bool restoredSound = restoredProgram == index
        && avRestoreGuardBank.load(std::memory_order_relaxed) == avBankVoicesHash(currentCart);
    const int selectedLayer = avSelectedLayer.load(std::memory_order_acquire);
    const auto& layerSettings = avLayerSettings[size_t(selectedLayer)];
    const float keptUnison = layerSettings.unison.load(), keptRandom = layerSettings.randomPitch.load();
    const int keptMono = layerSettings.mono.load();

    Program program = currentCart.unpackProgram(index);
    applyProgram(program);
    currentProgram = index;
    // Layer mode: a DX7II voice brings its unison, random pitch and mono
    // (from the bank's AMEM) to the selected layer; applyProgram cleared them.
    // A host re-selecting the restored sound keeps the session's.
    if (isAVLayerModeOn()) {
        if (restoredSound) {
            auto& settings = avLayerSettings[size_t(selectedLayer)];
            settings.unison.store(keptUnison);
            settings.randomPitch.store(keptRandom);
            settings.mono.store(keptMono);
        } else if (const auto* amem = currentCart.getAmemVoice(index)) {
            setAVLayerVoiceExtras(selectedLayer, amem);
        }
    }
    avKnownPresetOrigin.store(true, std::memory_order_relaxed);
    avSoundFromBankSlot.store(true, std::memory_order_relaxed);

    if (! restoredSound)
        applyAVSoundSettings(index);
    // The settings are part of the sound just loaded: it is not "Modified".
    captureAVPresetBaseline();
}

const String DexedAudioProcessor::getProgramName(int index) {
    index = juce::jlimit(0, 31, index);
    return programNames[index];
}

void DexedAudioProcessor::changeProgramName(int index, const String& newName) {
}

void DexedAudioProcessor::setDxParameter(int offset, uint8_t value) {
    if (offset == IDs::on.pos) {
        for (int op = 0; op < 6; ++op)
            if (auto* parameter = parameters.getParameter(IDs::on.op(op).name))
                parameter->setValueNotifyingHost((value & (1U << (5 - op))) != 0 ? 1.0f : 0.0f);
        return;
    }
    // Route the change through the APVTS parameter: the mapped listener updates
    // the model/voice (and echoes sysex out via setDxValue) while the bound UI
    // control follows automatically. Uses the same value conversion as
    // Program::pushToParameters(). A few offsets (e.g. osc key sync, operator
    // on/off) have no dedicated parameter and fall back to a direct model write.
    for (const auto &param : dxParameters) {
        if ( param.pos == offset ) {
            if ( juce::RangedAudioParameter *p = parameters.getParameter(param.name) ) {
                float displayValue = static_cast<float>(value - param.displayOffset);
                p->beginChangeGesture();
                p->setValueNotifyingHost(p->convertTo0to1(displayValue));
                p->endChangeGesture();
                return;
            }
        }
    }
    setDxValue(offset, value);
}

void DexedAudioProcessor::setDxValue(int offset, int v) {
    if (offset < 0)
        return;

    if ( activeProgram[offset] != v ) {
        activeProgram[offset] = v;
    } else {
        return;
    }

    refreshVoice.set(true);

    // MIDDLE C (transpose)
    if (offset == 144)
        panicForSoundChange();

    if (!sendSysexChange)
        return;

    uint8 msg[7] = { 0xF0, 0x43, 0x10, offset > 127, 0, (uint8) v, 0xF7 };
    msg[2] = 0x10 | sysexComm.getChl();
    msg[4] = offset & 0x7F;

    if ( sysexComm.isOutputActive() ) {
        //TRACE("SENDING SYSEX: %.2X%.2X %.2X%.2X %.2X%.2X %.2X", msg[0], msg[1], msg[2], msg[3], msg[4], msg[5], msg[6]);
        sysexComm.send(MidiMessage(msg,7));
    }
}
