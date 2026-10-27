// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../source/PluginProcessor.h"
#include "../source/parameter/Model.h"
#include "../source/midi/MidiCCHandler.h"
#include "../source/ui/AVDexedPeakMeter.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <set>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

float parameterValue(DexedAudioProcessor& processor, const juce::String& id)
{
    auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(processor.parameters.getParameter(id));
    return parameter == nullptr ? 0.0f : parameter->convertFrom0to1(parameter->getValue());
}

bool setParameterValue(DexedAudioProcessor& processor, const juce::String& id, float value)
{
    auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(processor.parameters.getParameter(id));
    if (parameter == nullptr)
        return false;
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    return true;
}

juce::MemoryBlock encodeTree(const juce::ValueTree& tree)
{
    juce::MemoryBlock bytes;
    if (auto xml = tree.createXml())
        juce::AudioProcessor::copyXmlToBinary(*xml, bytes);
    return bytes;
}

bool testCompleteVoiceParameterMapping()
{
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    processor.applyProgram(Program::initialProgram());

    std::set<int> mappedOffsets;
    bool mappingIsComplete = dxParameters.size() == 145;
    for (const auto& meta : dxParameters) {
        mappingIsComplete &= meta.pos >= 0 && meta.pos < Program::PROGRAM_SIZE;
        mappingIsComplete &= mappedOffsets.insert(meta.pos).second;
        auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(processor.parameters.getParameter(meta.name));
        if (parameter == nullptr) {
            mappingIsComplete = false;
            continue;
        }

        const auto minValue = parameter->convertFrom0to1(0.0f);
        const auto maxValue = parameter->convertFrom0to1(1.0f);
        const auto displayValue = std::round((minValue + maxValue) * 0.5f);
        parameter->setValueNotifyingHost(parameter->convertTo0to1(displayValue));
        const auto expected = juce::jlimit(0, 127,
            static_cast<int>(displayValue) + meta.displayOffset);
        if (meta.pos >= 0 && meta.pos < Program::PROGRAM_SIZE)
            mappingIsComplete &= processor.activeProgram.getRawData()[meta.pos] == expected;
    }

    bool operatorSwitchesMap = true;
    for (int op = 0; op < 6; ++op) {
        auto* parameter = processor.parameters.getParameter(IDs::on.op(op).name);
        if (parameter == nullptr) {
            operatorSwitchesMap = false;
            continue;
        }
        const bool enabled = (op % 2) == 0;
        parameter->setValueNotifyingHost(enabled ? 1.0f : 0.0f);
        const auto mask = static_cast<uint8_t>(1U << (5 - op));
        operatorSwitchesMap &= ((processor.activeProgram.getRawData()[155] & mask) != 0) == enabled;
    }

    return expect(mappingIsComplete, "all 145 DX voice offsets have one APVTS mapping, including pitch levels and oscillator sync")
        && expect(operatorSwitchesMap, "all six operator enable parameters map to the packed voice flags");
}

bool testStateRoundTripAndInvalidState()
{
    DexedAudioProcessor source(DexedAudioProcessor::TestMode{});
    DexedAudioProcessor restored(DexedAudioProcessor::TestMode{});

    source.applyProgram(Program::initialProgram());
    source.activeProgram.setName("STATE TEST");
    source.setCurrentProgramSelection(7);
    const bool sourceParametersSet = setParameterValue(source, IDs::masterVolume.name, 0.37f)
        && setParameterValue(source, IDs::pan.name, 0.72f)
        && setParameterValue(source, IDs::reverbAmount.name, 0.46f)
        && setParameterValue(source, IDs::delayTimeLeft.name, 327.0f)
        && setParameterValue(source, IDs::chorusAmount.name, 0.61f);

    juce::MemoryBlock snapshot;
    source.getStateInformation(snapshot);
    restored.applyProgram(Program::initialProgram());
    restored.activeProgram.setName("BEFORE");
    restored.setCurrentProgramSelection(2);
    restored.setStateInformation(snapshot.getData(), static_cast<int>(snapshot.getSize()));

    const bool roundTrip = sourceParametersSet
        && restored.getAVBankProgram() == 7
        && restored.activeProgram.getName() == "STATE TEST"
        && std::abs(parameterValue(restored, IDs::masterVolume.name) - 0.37f) < 0.002f
        && std::abs(parameterValue(restored, IDs::pan.name) - 0.72f) < 0.002f
        && std::abs(parameterValue(restored, IDs::reverbAmount.name) - 0.46f) < 0.002f
        && std::abs(parameterValue(restored, IDs::delayTimeLeft.name) - 327.0f) < 0.02f
        && std::abs(parameterValue(restored, IDs::chorusAmount.name) - 0.61f) < 0.002f;

    const auto beforeInvalidValue = parameterValue(restored, IDs::masterVolume.name);
    const auto beforeInvalidName = restored.activeProgram.getName();
    auto xml = juce::AudioProcessor::getXmlFromBinary(snapshot.getData(), static_cast<int>(snapshot.getSize()));
    auto invalidTree = juce::ValueTree::fromXml(*xml);
    auto session = invalidTree.getChildWithName("session");
    session.setProperty("voice", "invalid-base64", nullptr);
    const auto invalidSnapshot = encodeTree(invalidTree);
    restored.setStateInformation(invalidSnapshot.getData(), static_cast<int>(invalidSnapshot.getSize()));

    const bool invalidLeavesActiveState = std::abs(parameterValue(restored, IDs::masterVolume.name) - beforeInvalidValue) < 0.002f
        && restored.activeProgram.getName() == beforeInvalidName
        && restored.getAVBankProgram() == 7;

    return expect(roundTrip, "state restores the edited voice name, selection, output controls, and FX values")
        && expect(invalidLeavesActiveState, "malformed voice payload does not change the active sound or selection");
}

bool testVersion100ParameterMigration()
{
    DexedAudioProcessor source(DexedAudioProcessor::TestMode{});
    DexedAudioProcessor restored(DexedAudioProcessor::TestMode{});
    setParameterValue(source, IDs::masterVolume.name, 0.28f);

    juce::MemoryBlock snapshot;
    source.getStateInformation(snapshot);
    auto xml = juce::AudioProcessor::getXmlFromBinary(snapshot.getData(), static_cast<int>(snapshot.getSize()));
    auto oldTree = juce::ValueTree::fromXml(*xml);
    oldTree.setProperty(IDs::version, "1.0.0", nullptr);
    oldTree.setProperty("schemaVersion", 0, nullptr);
    if (auto session = oldTree.getChildWithName("session"); session.isValid())
        oldTree.removeChild(session, nullptr);

    const auto oldSnapshot = encodeTree(oldTree);
    restored.setStateInformation(oldSnapshot.getData(), static_cast<int>(oldSnapshot.getSize()));
    return expect(std::abs(parameterValue(restored, IDs::masterVolume.name) - 0.28f) < 0.002f,
                  "1.0.0 root states migrate their APVTS values without requiring newer session metadata");
}

bool testIndependentProcessorInstances()
{
    DexedAudioProcessor first(DexedAudioProcessor::TestMode{});
    DexedAudioProcessor second(DexedAudioProcessor::TestMode{});
    first.applyProgram(Program::initialProgram());
    second.applyProgram(Program::initialProgram());

    const bool setFirst = setParameterValue(first, IDs::masterVolume.name, 0.23f)
        && setParameterValue(first, IDs::pan.name, 0.18f)
        && setParameterValue(first, IDs::reverbAmount.name, 0.42f);
    const bool setSecond = setParameterValue(second, IDs::masterVolume.name, 0.81f)
        && setParameterValue(second, IDs::pan.name, 0.76f)
        && setParameterValue(second, IDs::reverbAmount.name, 0.09f);
    first.activeProgram.setName("INSTANCE A");
    second.activeProgram.setName("INSTANCE B");
    first.setCurrentProgramSelection(2);
    second.setCurrentProgramSelection(9);

    juce::MemoryBlock firstSnapshot;
    first.getStateInformation(firstSnapshot);
    setParameterValue(first, IDs::masterVolume.name, 0.5f);
    first.setStateInformation(firstSnapshot.getData(), static_cast<int>(firstSnapshot.getSize()));

    const bool firstRestored = std::abs(parameterValue(first, IDs::masterVolume.name) - 0.23f) < 0.002f
        && std::abs(parameterValue(first, IDs::pan.name) - 0.18f) < 0.002f
        && first.activeProgram.getName() == "INSTANCE A"
        && first.getAVBankProgram() == 2;
    const bool secondUnaffected = std::abs(parameterValue(second, IDs::masterVolume.name) - 0.81f) < 0.002f
        && std::abs(parameterValue(second, IDs::pan.name) - 0.76f) < 0.002f
        && std::abs(parameterValue(second, IDs::reverbAmount.name) - 0.09f) < 0.002f
        && second.activeProgram.getName() == "INSTANCE B"
        && second.getAVBankProgram() == 9;

    return expect(setFirst && setSecond, "two isolated processors accept independent musical settings")
        && expect(firstRestored, "restoring one processor restores its own saved context")
        && expect(secondUnaffected, "restoring one processor does not change another processor's state");
}

bool testOldSessionsResetMissingEffects()
{
    DexedAudioProcessor source(DexedAudioProcessor::TestMode{});
    DexedAudioProcessor restored(DexedAudioProcessor::TestMode{});
    setParameterValue(source, IDs::pitchEgLevel.idx(0).name, 67.0f);
    juce::MemoryBlock state;
    source.getStateInformation(state);
    auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    auto tree = juce::ValueTree::fromXml(*xml);
    tree.removeProperty("schemaVersion", nullptr);
    tree.removeChild(tree.getChildWithName("session"), nullptr);
    auto parameterTree = tree.getChildWithName(IDs::parameters);
    bool newParameters = false;
    std::set<juce::String> newParameterIds;
    for (const auto* abstractParameter : source.getParameters()) {
        const auto* parameter = dynamic_cast<const juce::RangedAudioParameter*>(abstractParameter);
        if (parameter == nullptr) continue;
        newParameters |= parameter->paramID == IDs::masterVolume.name;
        if (newParameters) newParameterIds.insert(parameter->paramID);
    }
    for (int index = 0; index < parameterTree.getNumChildren();) {
        auto child = parameterTree.getChild(index);
        if (newParameterIds.count(child.getProperty(IDs::id).toString()) != 0)
            parameterTree.removeChild(index, nullptr);
        else ++index;
    }
    setParameterValue(restored, IDs::masterVolume.name, 0.13f);
    setParameterValue(restored, IDs::pan.name, 0.0f);
    setParameterValue(restored, IDs::delayAmount.name, 0.8f);
    setParameterValue(restored, IDs::reverbAmount.name, 0.7f);
    setParameterValue(restored, IDs::chorusAmount.name, 0.6f);
    const auto oldState = encodeTree(tree);
    const bool loaded = restored.loadAVStateInformation(oldState.getData(), static_cast<int>(oldState.getSize()));
    return expect(loaded && parameterValue(restored, IDs::pitchEgLevel.idx(0).name) == 67.0f,
                  "a real 1.1.0 APVTS state restores its stored pitch-envelope content")
        && expect(parameterValue(restored, IDs::masterVolume.name) == 1.0f
                  && parameterValue(restored, IDs::pan.name) == 0.5f
                  && parameterValue(restored, IDs::delayAmount.name) == 0.0f
                  && parameterValue(restored, IDs::reverbAmount.name) == 0.0f
                  && parameterValue(restored, IDs::chorusAmount.name) == 0.0f,
                  "missing new parameters reset to neutral defaults after a previously wet sound");
}

bool testNativeContextAndDirtyRoundTrip()
{
    DexedAudioProcessor source(DexedAudioProcessor::TestMode{});
    DexedAudioProcessor restored(DexedAudioProcessor::TestMode{});
    source.applyProgram(Program::initialProgram());
    const auto unicodeName = juce::String::fromUTF8("Órbita oceánica 日本語");
    const auto missingFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile(juce::Uuid().toString() + ".dexedpreset");
    const auto nativeState = source.createAVNativePresetState(unicodeName, "av-test-id");
    const bool sourceUnaffected = source.getAVPresetId().isEmpty() && !source.isAVPresetModified();
    const bool loaded = restored.loadAVStateInformation(nativeState.getData(), static_cast<int>(nativeState.getSize()));
    restored.setAVPresetContext(unicodeName, "av-test-id", missingFile);
    const bool loadedClean = loaded && !restored.isAVPresetModified();
    setParameterValue(restored, IDs::pitchEgLevel.idx(2).name, 73.0f);
    juce::MemoryBlock editedSession;
    restored.getStateInformation(editedSession);
    source.loadAVStateInformation(editedSession.getData(), static_cast<int>(editedSession.getSize()));
    const bool nameAndDirtyRestored = source.getAVPresetName() == unicodeName
        && source.getAVPresetId() == "av-test-id" && source.getAVPresetPath() == missingFile
        && source.isAVPresetModified()
        && parameterValue(source, IDs::pitchEgLevel.idx(2).name) == 73.0f;
    source.setCurrentProgramSelection(17);
    const bool selectionCannotDestroyEdit = source.isAVPresetModified()
        && parameterValue(source, IDs::pitchEgLevel.idx(2).name) == 73.0f;
    source.captureAVPresetBaseline();
    return expect(sourceUnaffected, "creating a native save candidate does not mutate the active preset")
        && expect(loadedClean, "native preset contains its own clean content reference")
        && expect(nameAndDirtyRestored, "Unicode name, missing origin, and unsaved edited content survive a session roundtrip")
        && expect(selectionCannotDestroyEdit && !source.isAVPresetModified(),
                  "visible slot selection preserves edits and explicit successful-save baseline clears dirty");
}

bool testSemanticInvalidStatePreservesSound()
{
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    processor.applyProgram(Program::initialProgram());
    setParameterValue(processor, IDs::masterVolume.name, 0.37f);
    processor.setAVPresetContext("Active", "id", {});
    processor.captureAVPresetBaseline();
    juce::MemoryBlock snapshot;
    processor.getStateInformation(snapshot);
    auto xml = juce::AudioProcessor::getXmlFromBinary(snapshot.getData(), static_cast<int>(snapshot.getSize()));
    bool rejected = true;
    for (int invalidKind = 0; invalidKind < 4; ++invalidKind) {
        auto tree = juce::ValueTree::fromXml(*xml);
        auto parameters = tree.getChildWithName(IDs::parameters);
        if (invalidKind == 0) parameters.getChild(0).setProperty(IDs::value, "NaN", nullptr);
        if (invalidKind == 1) parameters.addChild(parameters.getChild(0).createCopy(), -1, nullptr);
        if (invalidKind == 2) tree.getChildWithName("tuning").setProperty("scl", "not a scale", nullptr);
        if (invalidKind == 3) tree.setProperty("schemaVersion", 999, nullptr);
        const auto malformed = encodeTree(tree);
        rejected &= !processor.loadAVStateInformation(malformed.getData(), static_cast<int>(malformed.getSize()));
        rejected &= std::abs(parameterValue(processor, IDs::masterVolume.name) - 0.37f) < 0.002f
            && processor.getAVPresetName() == "Active" && !processor.isAVPresetModified();
    }
    return expect(rejected, "nonfinite parameters, duplicate IDs, invalid tuning, and future schema preserve the active sound");
}

bool testSlotBankAndSysexRoundTrip()
{
    DexedAudioProcessor source(DexedAudioProcessor::TestMode{});
    source.applyProgram(Program::initialProgram());
    setParameterValue(source, IDs::pitchEgLevel.idx(1).name, 62.0f);
    setParameterValue(source, IDs::oscKeySync.name, 0.0f);
    setParameterValue(source, IDs::detune.op(3).name, -3.0f);
    source.activeProgram.setName("SLOT TEST");
    Cartridge bank;
    const auto initial = Program::initialProgram();
    for (int slot = 0; slot < 32; ++slot)
        bank.packProgram(initial.getRawData(), slot, initial.getName());
    bool slotPacked = bank.packProgram(source.activeProgram.getRawData(), 7, source.activeProgram.getName());
    const auto slot = bank.unpackProgram(7);
    slotPacked &= std::memcmp(slot.getRawData(), source.activeProgram.getRawData(), Program::PROGRAM_SIZE) == 0;

    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile(juce::Uuid().toString() + ".syx");
    const uint8_t performance[] = { 0xf0, 0x7d, 1, 2, 0xf7 };
    const uint8_t trailing[] = { 0xf0, 0x7d, 3, 0xf7 };
    juce::MemoryBlock wrapped(performance, sizeof(performance));
    wrapped.append(bank.getVoiceSysex(), SYSEX_SIZE);
    wrapped.append(trailing, sizeof(trailing));
    const bool initialWritten = file.replaceWithData(wrapped.getData(), wrapped.getSize());
    const bool saved = bank.saveVoice(file) == 1;
    Cartridge reloaded;
    const bool reloadedBank = reloaded.load(file) == 0;
    const auto reloadedSlot = reloaded.unpackProgram(7);
    juce::MemoryBlock fileBytes;
    file.loadFileAsData(fileBytes);
    const bool preservedWrapper = fileBytes.getSize() == wrapped.getSize()
        && std::memcmp(fileBytes.getData(), performance, sizeof(performance)) == 0
        && std::memcmp(static_cast<const uint8_t*>(fileBytes.getData()) + fileBytes.getSize() - sizeof(trailing), trailing, sizeof(trailing)) == 0;
    file.deleteFile();
    const auto sysex = source.activeProgram.toMidiMessage();
    Program decoded(sysex.getRawData() + 6);
    const bool voiceRoundTrip = sysex.getRawDataSize() == 163
        && sysexChecksum(sysex.getRawData() + 6, 155) == sysex.getRawData()[161]
        && std::memcmp(decoded.getRawData(), source.activeProgram.getRawData(), Program::PROGRAM_SIZE) == 0;
    return expect(slotPacked, "packing an edited slot keeps pitch levels, oscillator sync, detune, and voice name")
        && expect(initialWritten && saved && reloadedBank && preservedWrapper
                  && std::memcmp(reloadedSlot.getRawData(), source.activeProgram.getRawData(), Program::PROGRAM_SIZE) == 0,
                  "external bank save/reload retains the edited voice and surrounding SysEx performance messages")
        && expect(voiceRoundTrip, "single-voice SysEx export retains the edited voice and a valid checksum");
}

// Out-of-range bank values load as Dexed 1.1.0 loads them (the full
// field-by-field comparison is in tests/AVDexedVoiceTests.cpp); only the LFO
// speed and delay, which Dexed reads past its tables, are limited.
bool testOutOfRangeBankValuesAreClamped()
{
    Cartridge bank;
    const auto initial = Program::initialProgram();
    for (int slot = 0; slot < 32; ++slot)
        bank.packProgram(initial.getRawData(), slot, initial.getName());

    constexpr int slot = 5, op = 2, otherOp = 4;
    auto* voice = reinterpret_cast<uint8_t*>(bank.getRawVoice()) + slot * 128;
    voice[op * 17 + 14] = 102;            // output level, as in real DX7 banks
    voice[op * 17 + 0] = 0x80 | 120;      // EG rate 1 with the don't-care bit 7 set
    voice[op * 17 + 4] = 0x80 | 87;       // EG level 1, in range once bit 7 is masked
    voice[otherOp * 17 + 14] = 87;        // in-range output level
    voice[112] = 110;                     // LFO speed
    voice[117] = 60;                      // transpose

    const auto program = bank.unpackProgram(slot);
    const auto* data = program.getRawData();
    return expect(data[op * 21 + 16] == 102, "an output level of 102 loads as 102, as in Dexed 1.1.0 (not rescaled to 39)")
        && expect(data[op * 21 + 0] == 120, "an EG rate above 99 loads as the bank holds it, with bit 7 masked")
        && expect(data[op * 21 + 4] == 87 && data[otherOp * 21 + 16] == 87, "in-range bank values load unchanged")
        && expect(data[137] == 99 && data[144] == 60, "the LFO speed is limited to 99; the transpose loads as the bank holds it");
}

bool testLegacyBlobMigrationAndInvalidBlob()
{
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    const auto voice = Program::initialProgram();
    juce::XmlElement root("dexedState");
    root.setAttribute("gain", 0.43);
    root.setAttribute("currentProgram", 12);
    root.setAttribute("activeFileCartridge", "/tmp/av-dexed-missing-legacy-bank.syx");
    auto* blob = root.createNewChildElement("dexedBlob");
    juce::NamedValueSet attributes;
    attributes.set("program", juce::var(voice.getRawData(), Program::PROGRAM_SIZE));
    attributes.copyToXmlAttributes(*blob);
    juce::MemoryBlock legacy;
    juce::AudioProcessor::copyXmlToBinary(root, legacy);
    setParameterValue(processor, IDs::reverbAmount.name, 0.7f);
    const bool loaded = processor.loadAVStateInformation(legacy.getData(), static_cast<int>(legacy.getSize()));
    const bool preservedVoice = loaded && processor.getAVBankProgram() == 12
        && processor.activeProgram.getName() == voice.getName()
        && std::abs(parameterValue(processor, IDs::output.name) - 0.43f) < 0.002f
        && parameterValue(processor, IDs::reverbAmount.name) == 0.0f;
    const uint8_t shortBytes[] = { 0, 1, 2 };
    attributes.set("program", juce::var(shortBytes, sizeof(shortBytes)));
    attributes.copyToXmlAttributes(*blob);
    root.setAttribute("gain", 0.9);
    juce::MemoryBlock malformed;
    juce::AudioProcessor::copyXmlToBinary(root, malformed);
    const bool rejected = !processor.loadAVStateInformation(malformed.getData(), static_cast<int>(malformed.getSize()));
    return expect(preservedVoice, "legacy 1.0 binary voice migrates with a missing bank and neutral new effects")
        && expect(rejected && std::abs(parameterValue(processor, IDs::output.name) - 0.43f) < 0.002f
                  && processor.getAVBankProgram() == 12,
                  "short legacy voice data is rejected before any live parameter changes");
}

bool testAudioRoutingMidiAndSampleRates()
{
    bool audioIsValid = true;
    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0 }) {
        DexedAudioProcessor mono(DexedAudioProcessor::TestMode{});
        DexedAudioProcessor stereo(DexedAudioProcessor::TestMode{});
        auto layout = mono.getBusesLayout();
        layout.outputBuses.set(0, juce::AudioChannelSet::mono());
        audioIsValid &= mono.setBusesLayout(layout);
        mono.setRateAndBufferSizeDetails(sampleRate, 64);
        stereo.setRateAndBufferSizeDetails(sampleRate, 64);
        mono.prepareToPlay(sampleRate, 64);
        stereo.prepareToPlay(sampleRate, 64);
        for (const auto sampleCount : { 0, 1, 7, 64, 257, 13 }) {
            juce::AudioBuffer<float> monoBuffer(1, sampleCount), stereoBuffer(2, sampleCount);
            monoBuffer.clear(); stereoBuffer.clear();
            juce::MidiBuffer monoMidi, stereoMidi;
            if (sampleCount == 1) {
                monoMidi.addEvent(juce::MidiMessage::noteOn(1, 60, uint8_t(100)), 0);
                stereoMidi.addEvent(juce::MidiMessage::noteOn(1, 60, uint8_t(100)), 0);
            }
            mono.processBlock(monoBuffer, monoMidi);
            stereo.processBlock(stereoBuffer, stereoMidi);
            const auto monoPeaks = mono.consumeAVOutputPeaks();
            const auto stereoPeaks = stereo.consumeAVOutputPeaks();
            const float measured = sampleCount > 0 ? stereoBuffer.getMagnitude(0, 0, sampleCount) : 0.0f;
            audioIsValid &= monoPeaks.first == measured && monoPeaks.second == measured
                && stereoPeaks.first == measured && stereoPeaks.second == measured;
            audioIsValid &= stereo.consumeAVOutputPeaks() == std::pair<float, float>{0, 0};
            for (int sample = 0; sample < sampleCount; ++sample) {
                const auto value = stereoBuffer.getSample(0, sample);
                audioIsValid &= std::isfinite(value) && monoBuffer.getSample(0, sample) == value
                    && stereoBuffer.getSample(1, sample) == value;
            }
        }
        setParameterValue(stereo, IDs::masterVolume.name, DexedAudioProcessor::AVMasterMaximumGain);
        for (int block = 0; block < 40; ++block) {
            juce::AudioBuffer<float> monoAudio(1, 64), stereoAudio(2, 64);
            monoAudio.clear(); stereoAudio.clear();
            juce::MidiBuffer monoEvents, stereoEvents;
            mono.processBlock(monoAudio, monoEvents);
            stereo.processBlock(stereoAudio, stereoEvents);
            if (block == 39)
                for (int sample = 0; sample < 64; ++sample)
                    audioIsValid &= std::abs(stereoAudio.getSample(0, sample)
                        - monoAudio.getSample(0, sample) * DexedAudioProcessor::AVMasterMaximumGain) < 0.0001f;
        }
        stereo.setAVKeyboardPitch(12345);
        stereo.setAVKeyboardMod(96);
        juce::AudioBuffer<float> audio(2, 64); audio.clear();
        juce::MidiBuffer events;
        stereo.processBlock(audio, events);
        audioIsValid &= stereo.getAVKeyboardPitch() == 12345 && stereo.getAVKeyboardMod() == 96
            && stereo.controllers.pitch_cc == 12345 && stereo.controllers.modwheel_cc == 96;
        events.addEvent(juce::MidiMessage::pitchWheel(1, 4096), 0);
        events.addEvent(juce::MidiMessage::controllerEvent(1, 1, 48), 0);
        stereo.processBlock(audio, events);
        audioIsValid &= stereo.getAVKeyboardPitch() == 4096 && stereo.getAVKeyboardMod() == 48;
        const uint8_t pitchLevelChange[] = { 0xf0, 0x43, 0x10, 1, 2, 78, 0xf7 };
        const uint8_t switchChange[] = { 0xf0, 0x43, 0x10, 1, 27, 0x15, 0xf7 };
        const uint8_t tooShort[] = { 0xf0, 0x43, 0xf7 };
        events.addEvent(juce::MidiMessage(pitchLevelChange, sizeof(pitchLevelChange)), 0);
        events.addEvent(juce::MidiMessage(switchChange, sizeof(switchChange)), 0);
        events.addEvent(juce::MidiMessage(tooShort, sizeof(tooShort)), 0);
        stereo.processBlock(audio, events);
        stereo.flushAVPendingSysExForTesting();
        audioIsValid &= parameterValue(stereo, IDs::pitchEgLevel.idx(0).name) == 78.0f
            && stereo.activeProgram.getRawData()[130] == 78;
        for (int op = 0; op < 6; ++op)
            audioIsValid &= (parameterValue(stereo, IDs::on.op(op).name) > 0.5f)
                == ((0x15 & (1 << (5 - op))) != 0);
        setParameterValue(stereo, IDs::reverbAmount.name, 0.6f);
        setParameterValue(stereo, IDs::delayAmount.name, 0.7f);
        setParameterValue(stereo, IDs::masterVolume.name, 0.0f);
        for (int block = 0; block < 40; ++block) {
            stereo.consumeAVOutputPeaks();
            events.clear(); stereo.processBlock(audio, events);
            if (block == 39)
            {
                audioIsValid &= stereo.consumeAVOutputPeaks() == std::pair<float, float>{0, 0};
                for (int channel = 0; channel < 2; ++channel)
                    for (int sample = 0; sample < 64; ++sample)
                        audioIsValid &= audio.getSample(channel, sample) == 0.0f;
            }
        }
        stereo.panic();
        events.clear(); stereo.processBlock(audio, events);
        audioIsValid &= !stereo.keyboardState.isNoteOn(1, 60);
        mono.releaseResources(); stereo.releaseResources();

        AVDexedFxChain chain;
        chain.setDelayAmount(0.7f); chain.setReverbAmount(0.6f); chain.setChorusAmount(0.4f);
        chain.setDelayFeedback(999.0f);
        chain.prepare(sampleRate, 64);
        for (const auto sampleCount : { 0, 1, 63, 257 }) {
            juce::AudioBuffer<float> block(2, sampleCount);
            block.clear();
            if (sampleCount > 0) block.setSample(0, 0, 0.8f);
            chain.process(block, sampleCount, std::numeric_limits<double>::quiet_NaN());
            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < sampleCount; ++sample)
                    audioIsValid &= std::isfinite(block.getSample(channel, sample));
        }
        chain.setDelayAmount(0.0f); chain.setReverbAmount(0.0f); chain.setChorusAmount(0.0f);
        chain.reset();
        audioIsValid &= !chain.hasDelayTail() && !chain.hasReverbTail();
    }
    std::cout << "Delay generated DSP storage per instance: " << AVDexedDelay::AVStorageBytes
              << " bytes; four instances: " << 4 * AVDexedDelay::AVStorageBytes << " bytes (plus wrapper/buffers).\n";
    AVDexedMeters::AVDexedPeakMeterState meter;
    meter.update(1.2f, 0.02f);
    audioIsValid &= meter.proportion() == 1.0f && meter.clipIndicatorAlpha() == 1.0f;
    for (int frame = 0; frame < 300; ++frame) meter.update(0.0f, 0.02f);
    audioIsValid &= meter.proportion() == 0.0f && meter.clipIndicatorAlpha() == 0.0f;
    meter.update(std::numeric_limits<float>::quiet_NaN(), 0.02f);
    audioIsValid &= std::isfinite(meter.displayedDecibels());
    return expect(audioIsValid, "mono/stereo dry, +24 dB master, exact mute with tails, stereo output peak/clip/release, MIDI/SysEx, Panic, variable/zero blocks and 44.1/48/96 kHz FX stay valid");
}

bool testMidiLearnCC127AndSessionRestore()
{
    DexedAudioProcessor source(DexedAudioProcessor::TestMode{}), restored(DexedAudioProcessor::TestMode{});
    bool completedOnUI = false;
    source.midiCCMapper->startMapping(source.parameters.getParameter(IDs::masterVolume.name),
                                     [&completedOnUI] { completedOnUI = true; });
    source.midiCCMapper->processControlChange(127, 80);
    const bool deferredCompletion = !completedOnUI
        && source.midiCCMapper->getAVMappingParameterId(127) == IDs::masterVolume.name;
    source.midiCCMapper->processControlChange(127, 127);
    const bool topCCControlsParameter = std::abs(parameterValue(source, IDs::masterVolume.name)
        - DexedAudioProcessor::AVMasterMaximumGain) < 0.0001f;
    juce::MemoryBlock state;
    source.getStateInformation(state);
    const bool restoredState = restored.loadAVStateInformation(state.getData(), static_cast<int>(state.getSize()));
    restored.midiCCMapper->processControlChange(127, 0);
    const bool restoredMappingWorks = restoredState
        && restored.midiCCMapper->getMapping(IDs::masterVolume.name) == 127
        && parameterValue(restored, IDs::masterVolume.name) == 0.0f;
    source.midiCCMapper->commitAVPendingMapping();
    source.midiCCMapper->removeMapping(127);
    source.midiCCMapper->processControlChange(127, 0);
    source.midiCCMapper->processControlChange(128, 127);
    source.midiCCMapper->processControlChange(-1, 127);
    const bool removedMappingStopsControl = std::abs(parameterValue(source, IDs::masterVolume.name)
        - DexedAudioProcessor::AVMasterMaximumGain) < 0.0001f;
    // Cancelling or retargeting after audio sees a CC must not commit a stale Learn.
    source.midiCCMapper->startMapping(source.parameters.getParameter(IDs::pan.name), {});
    const auto wheel = juce::MidiMessage::controllerEvent(1, 1, 20);
    source.setRateAndBufferSizeDetails(48000.0, 64);
    source.prepareToPlay(48000.0, 64);
    juce::AudioBuffer<float> wheelAudio(2, 64); wheelAudio.clear();
    juce::MidiBuffer wheelEvents; wheelEvents.addEvent(wheel, 0);
    source.processBlock(wheelAudio, wheelEvents);
    source.releaseResources();
    const bool standardCCCanLearn = source.midiCCMapper->getMapping(IDs::pan.name) == 1;
    source.midiCCMapper->cancelMapping();
    const bool cancelledLearnIsGone = source.midiCCMapper->getMapping(IDs::pan.name) == -1;
    source.midiCCMapper->startMapping(source.parameters.getParameter(IDs::pan.name), {});
    source.midiCCMapper->processControlChange(9, 30);
    source.midiCCMapper->startMapping(source.parameters.getParameter(IDs::reverbAmount.name), {});
    source.midiCCMapper->processControlChange(10, 30);
    source.midiCCMapper->commitAVPendingMapping();
    const bool retargetIsCorrect = source.midiCCMapper->getMapping(IDs::pan.name) == -1
        && source.midiCCMapper->getMapping(IDs::reverbAmount.name) == 10;
    return expect(standardCCCanLearn && cancelledLearnIsGone && retargetIsCorrect, "standard CCs can Learn; cancellation and retarget do not commit stale mappings")
        && expect(deferredCompletion && completedOnUI, "MIDI Learn publishes its CC on audio and completes on the UI stage")
        && expect(topCCControlsParameter && restoredMappingWorks,
                  "CC127 operates a parameter and survives saving before the UI Learn callback, then restoring without an editor")
        && expect(removedMappingStopsControl, "removing a mapping stops its control and out-of-range CCs are ignored");
}

bool testDeferredSysexWithoutEditorAndOutput()
{
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    processor.setRateAndBufferSizeDetails(48000.0, 64);
    processor.prepareToPlay(48000.0, 64);
    juce::AudioBuffer<float> audio(2, 64); audio.clear();
    juce::MidiBuffer events;
    const auto before = parameterValue(processor, IDs::pitchEgLevel.idx(0).name);
    for (int value = 80; value < 89; ++value) {
        const uint8_t change[] = { 0xf0, 0x43, 0x10, 1, 2, static_cast<uint8_t>(value), 0xf7 };
        events.addEvent(juce::MidiMessage(change, sizeof(change)), 0);
    }
    processor.processBlock(audio, events);
    const bool deferred = parameterValue(processor, IDs::pitchEgLevel.idx(0).name) == before;
    processor.flushAVPendingSysExForTesting();
    const bool acceptedBoundedMessages = parameterValue(processor, IDs::pitchEgLevel.idx(0).name) == 87.0f
        && processor.getAVDroppedSysExCount() == 1;
    auto outgoing = processor.activeProgram.toMidiMessage();
    const bool queued = processor.sysexComm.send(outgoing) == 0;
    processor.sysexComm.flushAVPendingOutputForTesting();
    events.clear(); processor.processBlock(audio, events);
    bool outputMatches = events.getNumEvents() == 1;
    if (outputMatches) {
        const auto message = (*events.begin()).getMessage();
        outputMatches = message.getRawDataSize() == outgoing.getRawDataSize()
            && std::memcmp(message.getRawData(), outgoing.getRawData(), static_cast<size_t>(outgoing.getRawDataSize())) == 0;
    }
    processor.releaseResources();
    return expect(deferred && acceptedBoundedMessages,
                  "SysEx editing without an editor defers parsing outside render and bounded overflow reports discarded packets")
        && expect(queued && outputMatches, "prepared SysEx output reaches the host MIDI buffer with its complete voice data");
}

bool testOperatorGlowFollowsAudibleCarriers()
{
    // Algorithm 15: OP1 and OP3 are carriers; OP5 only adds into a modulation
    // bus. OP5 sustains fully and releases slowly, so it must never keep a
    // voice "playing" or lit once the carrier is silent.
    const auto makeProgram = [](bool carrierSustains) {
        uint8_t raw[Program::PROGRAM_SIZE];
        std::memcpy(raw, Program::initialProgram().getRawData(), sizeof(raw));
        raw[134] = 14;
        raw[21 + 3] = 10;       // OP5 R4: slow release
        raw[21 + 16] = 90;      // OP5 output level
        raw[105 + 2] = 80;      // OP1 R3: fast decay
        raw[105 + 6] = carrierSustains ? 99 : 0;   // OP1 L3
        return Program(raw);
    };
    const auto run = [](DexedAudioProcessor& processor, int blocks, bool noteOn, bool noteOff) {
        for (int block = 0; block < blocks; ++block) {
            juce::AudioBuffer<float> audio(2, 480); audio.clear();
            juce::MidiBuffer midi;
            if (block == 0 && noteOn) midi.addEvent(juce::MidiMessage::noteOn(1, 60, uint8_t(100)), 0);
            if (block == 0 && noteOff) midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            processor.processBlock(audio, midi);
        }
    };

    DexedAudioProcessor released(DexedAudioProcessor::TestMode{});
    released.applyProgram(makeProgram(true));
    released.setRateAndBufferSizeDetails(48000.0, 480);
    released.prepareToPlay(48000.0, 480);
    run(released, 20, true, false);
    const bool litWhileHeld = released.peekVoiceStatus() && released.getAVActiveVoices() == 1;
    run(released, 30, false, true);
    const bool darkAfterRelease = !released.peekVoiceStatus() && released.getAVActiveVoices() == 0;

    DexedAudioProcessor held(DexedAudioProcessor::TestMode{});
    held.applyProgram(makeProgram(false));
    held.setRateAndBufferSizeDetails(48000.0, 480);
    held.prepareToPlay(48000.0, 480);
    run(held, 200, true, false);
    const bool darkOnceSilent = !held.peekVoiceStatus();

    return expect(litWhileHeld, "a held note lights its operators and counts as one voice")
        && expect(darkAfterRelease, "a modulator that adds into a bus does not keep a released voice playing or lit")
        && expect(darkOnceSilent, "a held key whose carriers decayed to silence leaves the operators dark");
}

//==============================================================================
// Per-sound modern settings (embedded in the .syx) and "Guardar" into a bank.

/** A temporary data folder (the library root) for one test. The processor's
    static folders point at it meanwhile and are restored afterwards. */
struct AVScratchLibrary {
    juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("avdexed-state-" + juce::Uuid().toString());
    const juce::File oldAppDirectory = DexedAudioProcessor::dexedAppDir;
    const juce::File oldCartDirectory = DexedAudioProcessor::dexedCartDir;

    AVScratchLibrary() {
        root.createDirectory();
        DexedAudioProcessor::dexedAppDir = root;
        DexedAudioProcessor::dexedCartDir = AVDexedLibrary::rootFor(root);
    }
    ~AVScratchLibrary() {
        DexedAudioProcessor::dexedAppDir = oldAppDirectory;
        DexedAudioProcessor::dexedCartDir = oldCartDirectory;
        if (root.getFileName().startsWith("avdexed-state-")
            && root.isAChildOf(juce::File::getSpecialLocation(juce::File::tempDirectory)))
            root.deleteRecursively();
    }
};

/** 32 different voices (the name and algorithm change), so every slot has its own hash. */
Cartridge makeDistinctBank(int variant)
{
    Cartridge bank;
    for (int slot = 0; slot < 32; ++slot) {
        uint8_t raw[Program::PROGRAM_SIZE];
        std::memcpy(raw, Program::initialProgram().getRawData(), sizeof(raw));
        raw[134] = static_cast<uint8_t>((slot + variant) % 32);
        const Program voice(raw);
        bank.packProgram(voice.getRawData(), slot, "B" + juce::String(variant) + " S" + juce::String(slot).paddedLeft('0', 2));
    }
    return bank;
}

const uint8_t* slotBytes(const Cartridge& bank, int slot)
{
    return reinterpret_cast<const uint8_t*>(bank.getVoiceSysex()) + 6 + slot * 128;
}

bool writeBankFile(const juce::File& file, const Cartridge& bank, const AVDexedLibrary::BankSettings& settings,
                   const juce::MemoryBlock& around = {})
{
    const auto bytes = AVDexedLibrary::composeBankFile(around, bank, settings);
    return file.getParentDirectory().createDirectory().wasOk() && file.replaceWithData(bytes.getData(), bytes.getSize());
}

bool near(float value, float expected, float tolerance = 0.002f) { return std::abs(value - expected) < tolerance; }

bool hasDefaultSoundSettings(DexedAudioProcessor& processor)
{
    return near(parameterValue(processor, IDs::masterVolume.name), 1.0f, 0.0001f)
        && near(parameterValue(processor, IDs::pan.name), 0.5f, 0.0001f)
        && near(parameterValue(processor, IDs::reverbAmount.name), 0.0f, 0.0001f)
        && parameterValue(processor, IDs::chorusMode.name) == 0.0f
        && parameterValue(processor, IDs::avReverbBypass.name) == 0.0f;
}

bool testSoundSettingsFollowTheirVoice()
{
    AVScratchLibrary library;
    const auto bank = makeDistinctBank(1);
    AVDexedLibrary::BankSettings settings;
    settings[3] = { AVDexedLibrary::voiceHash(bank, 3),
                    { { "masterVolume", 0.5f }, { "reverbAmount", 0.4f }, { "chorusMode", 2.0f },
                      { "avReverbBypass", 1.0f }, { "aFutureParameter", 3.0f } } };
    const auto file = library.root.getChildFile("Mis bancos").getChildFile("Bank.syx");
    const bool written = writeBankFile(file, bank, settings);

    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    processor.setActiveBank(bank, file);
    const bool bankOnly = processor.activeFileCartridge == file && processor.getAVBankProgram() == 0
        && hasDefaultSoundSettings(processor);

    processor.setAVBankProgram(3);
    const bool applied = near(parameterValue(processor, IDs::masterVolume.name), 0.5f)
        && near(parameterValue(processor, IDs::reverbAmount.name), 0.4f)
        && parameterValue(processor, IDs::chorusMode.name) == 2.0f
        && parameterValue(processor, IDs::avReverbBypass.name) == 1.0f
        && !processor.isAVPresetModified();

    processor.setAVBankProgram(4);
    const bool defaults = hasDefaultSoundSettings(processor) && !processor.isAVPresetModified();

    // The bank manager moved slot 3's voice to slot 10 and put another voice in slot 3.
    Cartridge source = bank, moved = bank;
    moved.setPackedProgram(10, source.getRawVoice() + 3 * 128);
    moved.setPackedProgram(3, source.getRawVoice() + 11 * 128);
    const auto movedFile = file.getSiblingFile("Moved.syx");
    const bool movedWritten = writeBankFile(movedFile, moved, settings);
    processor.setActiveBank(moved, movedFile);
    processor.setAVBankProgram(10);
    const bool followedVoice = near(parameterValue(processor, IDs::masterVolume.name), 0.5f)
        && near(parameterValue(processor, IDs::reverbAmount.name), 0.4f);
    processor.setAVBankProgram(3);
    const bool staleSlotIgnored = hasDefaultSoundSettings(processor);

    // A MIDI program change takes the same path, inside render.
    processor.setRateAndBufferSizeDetails(48000.0, 64);
    processor.prepareToPlay(48000.0, 64);
    juce::AudioBuffer<float> audio(2, 64); audio.clear();
    juce::MidiBuffer events;
    events.addEvent(juce::MidiMessage::programChange(1, 10), 0);
    processor.processBlock(audio, events);
    const bool midiProgramChange = processor.getAVBankProgram() == 10
        && near(parameterValue(processor, IDs::masterVolume.name), 0.5f) && !processor.isAVPresetModified();
    processor.releaseResources();

    // Selecting without loading (session restore, visible slot) applies nothing.
    processor.setCurrentProgramSelection(4);
    const bool selectionOnly = processor.getAVBankProgram() == 4
        && near(parameterValue(processor, IDs::masterVolume.name), 0.5f);

    // A bank without file has no settings.
    processor.setActiveBank(bank, juce::File());
    processor.setAVBankProgram(3);
    const bool noFileNoSettings = hasDefaultSoundSettings(processor) && !processor.hasExternalCartridgeContext();

    return expect(written && movedWritten && bankOnly, "setActiveBank installs the bank and its file without touching the program or parameters")
        && expect(applied, "a voice's embedded settings apply on setCurrentProgram, unknown ids are ignored, the sound is not Modified")
        && expect(defaults, "a voice without settings gets the neutral defaults")
        && expect(followedVoice && staleSlotIgnored, "settings follow their voice to another slot and never land on a different voice")
        && expect(midiProgramChange, "a MIDI program change in render applies the settings and recaptures the baseline")
        && expect(selectionOnly, "setCurrentProgramSelection applies nothing")
        && expect(noFileNoSettings, "a bank without file plays every sound with the defaults");
}

bool testRestoreKeepsSessionValues()
{
    AVScratchLibrary library;
    // Where the 0.2.0 migration put Dexed_01; 0.1.0 sessions point into Cartridges/.
    const auto bank = makeDistinctBank(2);
    AVDexedLibrary::BankSettings settings;
    settings[3] = { AVDexedLibrary::voiceHash(bank, 3), { { "masterVolume", 0.5f } } };
    const auto file = library.root.getChildFile(AVDexedLibrary::factoryFolderName).getChildFile("Dexed_01.syx");
    const bool written = writeBankFile(file, bank, settings);

    DexedAudioProcessor source(DexedAudioProcessor::TestMode{});
    source.setActiveBank(bank, file);
    source.setAVBankProgram(3);
    setParameterValue(source, IDs::masterVolume.name, 0.77f);
    juce::MemoryBlock state;
    source.getStateInformation(state);
    auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    auto tree = juce::ValueTree::fromXml(*xml);
    tree.getChildWithName("session").setProperty("sourcePath",
        library.root.getChildFile("Cartridges").getChildFile("Dexed_01.syx").getFullPathName(), nullptr);
    const auto oldSession = encodeTree(tree);

    DexedAudioProcessor restored(DexedAudioProcessor::TestMode{});
    restored.setStateInformation(oldSession.getData(), static_cast<int>(oldSession.getSize()));
    const bool restoredValues = restored.getAVBankProgram() == 3
        && near(parameterValue(restored, IDs::masterVolume.name), 0.77f)
        && restored.activeFileCartridge == file;

    // A host's own program choice never changes the sound, restored or not.
    restored.setCurrentProgram(3);
    restored.setCurrentProgram(5);
    const bool hostIgnored = restored.getAVBankProgram() == 3 && near(parameterValue(restored, IDs::masterVolume.name), 0.77f)
        && source.isAVPresetModified() && restored.isAVPresetModified();

    // A Program Change re-selects the restored program right away (a track's
    // program when the project opens): the session wins, once.
    restored.setAVBankProgram(3);
    const bool skippedOnce = near(parameterValue(restored, IDs::masterVolume.name), 0.77f) && !restored.isAVPresetModified();
    restored.setAVBankProgram(3);
    const bool appliedAfterwards = near(parameterValue(restored, IDs::masterVolume.name), 0.5f);

    // The guard expires after about a second of the 30 Hz timer...
    restored.setStateInformation(oldSession.getData(), static_cast<int>(oldSession.getSize()));
    for (int tick = 0; tick < 30; ++tick)
        restored.runAVTimerTickForTesting();
    restored.setAVBankProgram(3);
    const bool expired = near(parameterValue(restored, IDs::masterVolume.name), 0.5f);

    // ...and any other program change consumes it.
    restored.setStateInformation(oldSession.getData(), static_cast<int>(oldSession.getSize()));
    restored.setAVBankProgram(4);
    restored.setAVBankProgram(3);
    const bool consumed = near(parameterValue(restored, IDs::masterVolume.name), 0.5f);

    // A preset loaded from the UI is not a host restore: nothing is skipped.
    restored.loadAVStateInformation(oldSession.getData(), static_cast<int>(oldSession.getSize()));
    const bool presetValues = near(parameterValue(restored, IDs::masterVolume.name), 0.77f);
    restored.setAVBankProgram(3);
    const bool presetNotGuarded = near(parameterValue(restored, IDs::masterVolume.name), 0.5f);

    return expect(written && restoredValues, "a restore keeps the session's values and maps a 0.1.0 Cartridges/ path into the library")
        && expect(hostIgnored, "a host's program choice leaves the restored sound as it was saved, \"Modified\" too")
        && expect(skippedOnce && appliedAfterwards, "the first re-selection of the restored program keeps the session values, the next applies the settings")
        && expect(expired && consumed, "the restore guard expires with the timer and is consumed by any program change")
        && expect(presetValues && presetNotGuarded, "loading a preset from the UI does not arm the restore guard");
}

bool testRestoreGuardOnlyForRestoredBank()
{
    AVScratchLibrary library;
    const auto folder = library.root.getChildFile("Mis bancos");
    const auto bank = makeDistinctBank(10), other = makeDistinctBank(11);
    AVDexedLibrary::BankSettings settings, otherSettings, copySettings;
    settings[3] = { AVDexedLibrary::voiceHash(bank, 3), { { "masterVolume", 0.5f } } };
    otherSettings[3] = { AVDexedLibrary::voiceHash(other, 3), { { "reverbAmount", 0.4f } } };
    copySettings[3] = { AVDexedLibrary::voiceHash(bank, 3), { { "masterVolume", 0.2f } } };
    const auto file = folder.getChildFile("Restaurado.syx");
    const auto otherFile = folder.getChildFile("Otro.syx");
    const auto copyFile = folder.getChildFile("Copia.syx");   // the same voices under another name
    const bool written = writeBankFile(file, bank, settings) && writeBankFile(otherFile, other, otherSettings)
        && writeBankFile(copyFile, bank, copySettings);

    DexedAudioProcessor source(DexedAudioProcessor::TestMode{});
    source.setActiveBank(bank, file);
    source.setAVBankProgram(3);
    setParameterValue(source, IDs::masterVolume.name, 0.77f);
    juce::MemoryBlock session;
    source.getStateInformation(session);

    // Within the guard's second the user plays slot 3 of another bank (the browser).
    DexedAudioProcessor restored(DexedAudioProcessor::TestMode{});
    restored.setStateInformation(session.getData(), static_cast<int>(session.getSize()));
    restored.setActiveBank(other, otherFile);
    restored.setAVBankProgram(3);
    const bool otherBankOwnSettings = near(parameterValue(restored, IDs::reverbAmount.name), 0.4f)
        && near(parameterValue(restored, IDs::masterVolume.name), 1.0f, 0.0001f) && !restored.isAVPresetModified();

    // A 32-voice SysEx bank arrives: its slot gets the defaults.
    restored.setStateInformation(session.getData(), static_cast<int>(session.getSize()));
    restored.setActiveBank(other, juce::File());
    restored.setAVBankProgram(3);
    const bool sysexDefaults = hasDefaultSoundSettings(restored);

    // The restored voices only re-pointed to another file: the session still wins.
    restored.setStateInformation(session.getData(), static_cast<int>(session.getSize()));
    restored.setActiveBank(bank, copyFile);
    restored.setAVBankProgram(3);
    const bool repointedKept = near(parameterValue(restored, IDs::masterVolume.name), 0.77f);

    return expect(written && otherBankOwnSettings, "after a restore, the same slot of another bank gets that bank's settings")
        && expect(sysexDefaults, "after a restore, the same slot of a SysEx bank gets the defaults")
        && expect(repointedKept, "the restore guard survives a bank with the same voices re-pointed to another file");
}

bool testRestoreKeepsSaveTarget()
{
    AVScratchLibrary library;
    const auto bank = makeDistinctBank(9);
    const auto file = library.root.getChildFile("Mis bancos").getChildFile("Rhodes.syx");
    const bool written = writeBankFile(file, bank, {});
    const auto readBytes = [](const juce::File& f) { juce::MemoryBlock bytes; f.loadFileAsData(bytes); return bytes; };
    const auto original = readBytes(file);

    // A new sound designed from Init over slot 5: "Guardar" goes to Save As...
    DexedAudioProcessor source(DexedAudioProcessor::TestMode{});
    source.setActiveBank(bank, file);
    source.setAVBankProgram(5);
    source.resetToInitVoice();
    setParameterValue(source, IDs::pitchEgLevel.idx(0).name, 61.0f);
    const bool initGoesToSaveAs = !source.canSaveAVSoundToBank();
    juce::MemoryBlock initSession;
    source.getStateInformation(initSession);

    // ...also once the host reopens the project or duplicates the track.
    DexedAudioProcessor restored(DexedAudioProcessor::TestMode{});
    restored.setStateInformation(initSession.getData(), static_cast<int>(initSession.getSize()));
    const bool restoredInit = restored.activeFileCartridge == file && restored.getAVBankProgram() == 5
        && !restored.canSaveAVSoundToBank()
        && restored.saveAVSoundToBank("X") == DexedAudioProcessor::AVBankSaveResult::needsSaveAs
        && readBytes(file) == original;

    // A bank voice that was only edited still saves into its slot.
    source.setAVBankProgram(5);
    setParameterValue(source, IDs::pitchEgLevel.idx(0).name, 62.0f);
    juce::MemoryBlock editedSession;
    source.getStateInformation(editedSession);
    DexedAudioProcessor restoredEdit(DexedAudioProcessor::TestMode{});
    restoredEdit.setStateInformation(editedSession.getData(), static_cast<int>(editedSession.getSize()));
    const bool restoredEdited = restoredEdit.canSaveAVSoundToBank();

    // Sessions saved before 0.2.0 do not store it: the restored origin decides.
    const auto withoutFlag = [](const juce::MemoryBlock& state) {
        auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
        auto tree = juce::ValueTree::fromXml(*xml);
        tree.getChildWithName("session").removeProperty("avSoundFromBankSlot", nullptr);
        return encodeTree(tree);
    };
    const auto oldInit = withoutFlag(initSession), oldEdit = withoutFlag(editedSession);
    DexedAudioProcessor fromOld(DexedAudioProcessor::TestMode{});
    fromOld.setStateInformation(oldInit.getData(), static_cast<int>(oldInit.getSize()));
    const bool oldInitSaveAs = !fromOld.canSaveAVSoundToBank();
    fromOld.setStateInformation(oldEdit.getData(), static_cast<int>(oldEdit.getSize()));
    const bool oldEditSlot = fromOld.canSaveAVSoundToBank();

    return expect(written && initGoesToSaveAs && restoredInit,
                  "an Init sound restored by the host still goes to Save As and never overwrites the bank voice")
        && expect(restoredEdited, "an edited bank voice restored by the host still saves into its slot")
        && expect(oldInitSaveAs && oldEditSlot, "a session without the flag takes it from the restored origin");
}

bool testSaveSoundToBank()
{
    AVScratchLibrary library;
    const auto bank = makeDistinctBank(3);
    AVDexedLibrary::BankSettings settings;
    settings[3] = { AVDexedLibrary::voiceHash(bank, 3), { { "masterVolume", 0.5f } } };
    settings[5] = { AVDexedLibrary::voiceHash(bank, 5), { { "reverbAmount", 0.25f } } };
    // The bank file carries other SysEx messages around the bank (performance data).
    const uint8_t performance[] = { 0xf0, 0x7d, 1, 2, 0xf7 };
    const uint8_t trailing[] = { 0xf0, 0x7d, 3, 0xf7 };
    Cartridge wrappedBank = bank;
    juce::MemoryBlock around(performance, sizeof(performance));
    around.append(wrappedBank.getVoiceSysex(), SYSEX_SIZE);
    around.append(trailing, sizeof(trailing));
    const auto file = library.root.getChildFile("Mis bancos").getChildFile("Propio.syx");
    const bool written = writeBankFile(file, bank, settings, around);

    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    processor.setActiveBank(bank, file);
    processor.setAVBankProgram(3);
    const bool canSave = processor.canSaveAVSoundToBank();
    setParameterValue(processor, IDs::masterVolume.name, 0.3f);
    setParameterValue(processor, IDs::pan.name, 0.2f);
    setParameterValue(processor, IDs::pitchEgLevel.idx(0).name, 61.0f);
    const bool modifiedBefore = processor.isAVPresetModified();
    const auto captured = processor.captureAVSoundSettings();
    const bool capturedOnlyChanges = captured.values.size() == 2
        && near(captured.values.at("masterVolume"), 0.3f) && near(captured.values.at("pan"), 0.2f);

    const auto result = processor.saveAVSoundToBank("SAVED");
    const bool saved = result == DexedAudioProcessor::AVBankSaveResult::saved
        && !processor.isAVPresetModified() && processor.getAVBankProgram() == 3
        && processor.activeProgram.getName().trimEnd() == "SAVED"
        && processor.programNames[3].trimEnd() == "SAVED"
        && processor.activeFileCartridge == file;

    Cartridge onDisk;
    juce::MemoryBlock bytes;
    const bool reread = AVDexedLibrary::readBank(file, onDisk) && file.loadFileAsData(bytes);
    bool onlyThatSlot = reread;
    for (int slot = 0; slot < 32; ++slot)
        if (slot != 3)
            onlyThatSlot &= std::memcmp(slotBytes(onDisk, slot), slotBytes(bank, slot), 128) == 0;
    onlyThatSlot &= onDisk.unpackProgram(3).getRawData()[130] == 61 && onDisk.getProgramName(3).trimEnd() == "SAVED";
    const auto* raw = static_cast<const uint8_t*>(bytes.getData());
    const bool keptOtherMessages = bytes.getSize() > sizeof(performance) + SYSEX_SIZE + sizeof(trailing)
        && std::memcmp(raw, performance, sizeof(performance)) == 0
        && std::memcmp(raw + sizeof(performance) + SYSEX_SIZE, trailing, sizeof(trailing)) == 0;
    const auto stored = AVDexedLibrary::readEmbeddedSettings(file);
    const bool settingsStored = stored.size() == 2
        && stored.count(5) == 1 && stored.at(5).voice == AVDexedLibrary::voiceHash(bank, 5)
        && near(stored.at(5).values.at("reverbAmount"), 0.25f)
        && stored.count(3) == 1 && stored.at(3).voice == AVDexedLibrary::voiceHash(onDisk, 3)
        && stored.at(3).values.size() == 2
        && near(stored.at(3).values.at("masterVolume"), 0.3f) && near(stored.at(3).values.at("pan"), 0.2f);

    processor.setAVBankProgram(4);
    processor.setAVBankProgram(3);
    const bool roundTrip = near(parameterValue(processor, IDs::masterVolume.name), 0.3f)
        && near(parameterValue(processor, IDs::pan.name), 0.2f)
        && parameterValue(processor, IDs::pitchEgLevel.idx(0).name) == 61.0f
        && !processor.isAVPresetModified();

    // Only the settings changed: the voice bytes, and the favourites that use them, stay.
    const auto fingerprint = AVDexedLibrary::bankFingerprint(onDisk);
    setParameterValue(processor, IDs::reverbAmount.name, 0.6f);
    const bool savedSettingsOnly = processor.saveAVSoundToBank("SAVED") == DexedAudioProcessor::AVBankSaveResult::saved;
    Cartridge afterSettings;
    AVDexedLibrary::readBank(file, afterSettings);
    const auto storedAgain = AVDexedLibrary::readEmbeddedSettings(file);
    const bool voiceKept = savedSettingsOnly && AVDexedLibrary::bankFingerprint(afterSettings) == fingerprint
        && storedAgain.count(3) == 1 && near(storedAgain.at(3).values.at("reverbAmount"), 0.6f);

    // Back to the defaults: the slot's entry goes, the others stay.
    setParameterValue(processor, IDs::masterVolume.name, 1.0f);
    setParameterValue(processor, IDs::pan.name, 0.5f);
    setParameterValue(processor, IDs::reverbAmount.name, 0.0f);
    const bool savedDefaults = processor.saveAVSoundToBank("SAVED") == DexedAudioProcessor::AVBankSaveResult::saved;
    const auto storedDefaults = AVDexedLibrary::readEmbeddedSettings(file);
    const bool entryErased = savedDefaults && storedDefaults.count(3) == 0 && storedDefaults.count(5) == 1;

    return expect(written && canSave && modifiedBefore && capturedOnlyChanges,
                  "a bank in the library can be saved into and only non-default settings are captured, in real units")
        && expect(saved, "saving keeps the program, renames the voice and clears Modified")
        && expect(onlyThatSlot && keptOtherMessages, "saving patches only the current slot and keeps the file's other SysEx messages")
        && expect(settingsStored && roundTrip, "the saved settings carry the new voice hash, keep other slots and come back on load")
        && expect(voiceKept, "saving only new settings keeps the bank's voice bytes")
        && expect(entryErased, "a sound back at the defaults loses its settings entry");
}

bool testSaveNeedsSaveAs()
{
    AVScratchLibrary library;
    const auto bank = makeDistinctBank(4);
    const auto readBytes = [](const juce::File& file) { juce::MemoryBlock bytes; file.loadFileAsData(bytes); return bytes; };

    // The file changed on disk after it was loaded (another AV Dexed, Finder).
    const auto file = library.root.getChildFile("Mis bancos").getChildFile("Cambiado.syx");
    bool written = writeBankFile(file, bank, {});
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    processor.setActiveBank(bank, file);
    processor.setAVBankProgram(2);
    written &= writeBankFile(file, makeDistinctBank(5), {});
    const auto changedBytes = readBytes(file);
    setParameterValue(processor, IDs::masterVolume.name, 0.4f);
    const bool changedOnDisk = processor.canSaveAVSoundToBank()
        && processor.saveAVSoundToBank("X") == DexedAudioProcessor::AVBankSaveResult::needsSaveAs
        && readBytes(file) == changedBytes && processor.isAVPresetModified();

    // A read-only pack (factory sounds).
    const auto packFolder = library.root.getChildFile(AVDexedLibrary::factoryFolderName);
    const auto packFile = packFolder.getChildFile("Dexed_01.syx");
    written &= writeBankFile(packFile, bank, {})
        && AVDexedLibrary::writePackManifest(packFolder, "dexed-factory", AVDexedLibrary::factoryFolderName);
    const auto packBytes = readBytes(packFile);
    processor.setActiveBank(bank, packFile);
    const bool readOnly = !processor.canSaveAVSoundToBank()
        && processor.saveAVSoundToBank("X") == DexedAudioProcessor::AVBankSaveResult::needsSaveAs
        && readBytes(packFile) == packBytes;

    // A bank outside the library, and a bank without file.
    const auto outsideFolder = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("avdexed-state-outside-" + juce::Uuid().toString());
    const auto outsideFile = outsideFolder.getChildFile("Descargas.syx");
    written &= writeBankFile(outsideFile, bank, {});
    const auto outsideBytes = readBytes(outsideFile);
    processor.setActiveBank(bank, outsideFile);
    const bool outside = !processor.canSaveAVSoundToBank()
        && processor.saveAVSoundToBank("X") == DexedAudioProcessor::AVBankSaveResult::needsSaveAs
        && readBytes(outsideFile) == outsideBytes;
    outsideFile.deleteFile();
    outsideFolder.deleteFile();
    processor.setActiveBank(bank, juce::File());
    const bool noFile = !processor.canSaveAVSoundToBank()
        && processor.saveAVSoundToBank("X") == DexedAudioProcessor::AVBankSaveResult::needsSaveAs;

    return expect(written, "test banks are written")
        && expect(changedOnDisk, "a bank file changed on disk after loading is never overwritten: Save As")
        && expect(readOnly, "a bank in a read-only pack goes to Save As and stays untouched")
        && expect(outside && noFile, "a bank outside the library or without file goes to Save As");
}

bool testSaveKeepsOtherSlotsAsTheyWere()
{
    AVScratchLibrary library;
    auto bank = makeDistinctBank(7);
    // Slot 21 is a copy of slot 20 (same voice, same hash). Slot 3's entry is
    // stale: its voice was moved to slot 10 by the bank manager.
    bank.setPackedProgram(21, bank.getRawVoice() + 20 * 128);
    AVDexedLibrary::BankSettings settings;
    settings[20] = { AVDexedLibrary::voiceHash(bank, 20), { { "reverbAmount", 0.3f } } };
    settings[3] = { AVDexedLibrary::voiceHash(bank, 10), { { "masterVolume", 0.6f } } };
    const auto file = library.root.getChildFile("Mis bancos").getChildFile("Duplicados.syx");
    const bool written = writeBankFile(file, bank, settings);
    const auto fingerprint = AVDexedLibrary::bankFingerprint(bank);

    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    processor.setActiveBank(bank, file);

    // The copy plays with its twin's settings; saved back at the defaults it
    // keeps them (an empty entry of its own), and the twin keeps its reverb.
    processor.setAVBankProgram(21);
    const bool inherited = near(parameterValue(processor, IDs::reverbAmount.name), 0.3f);
    setParameterValue(processor, IDs::reverbAmount.name, 0.0f);
    const bool savedCopy = processor.saveAVSoundToBank({}) == DexedAudioProcessor::AVBankSaveResult::saved;
    processor.setAVBankProgram(20);
    const bool twinKept = near(parameterValue(processor, IDs::reverbAmount.name), 0.3f);
    processor.setAVBankProgram(21);
    const bool copyDry = hasDefaultSoundSettings(processor);
    const auto afterCopy = AVDexedLibrary::readEmbeddedSettings(file);
    const bool explicitEntry = afterCopy.count(21) == 1 && afterCopy.at(21).values.empty()
        && afterCopy.at(21).voice == AVDexedLibrary::voiceHash(bank, 21);

    // Saving slot 3 replaces its stale entry: the moved voice in slot 10
    // still finds its settings afterwards.
    processor.setAVBankProgram(3);
    const bool staleNotApplied = hasDefaultSoundSettings(processor);
    setParameterValue(processor, IDs::pan.name, 0.3f);
    const bool savedSlot3 = processor.saveAVSoundToBank(processor.activeProgram.getName()) == DexedAudioProcessor::AVBankSaveResult::saved;
    processor.setAVBankProgram(10);
    const bool movedKept = near(parameterValue(processor, IDs::masterVolume.name), 0.6f)
        && near(parameterValue(processor, IDs::pan.name), 0.5f);
    processor.setAVBankProgram(3);
    const bool slot3Own = near(parameterValue(processor, IDs::pan.name), 0.3f)
        && near(parameterValue(processor, IDs::masterVolume.name), 1.0f);

    Cartridge onDisk;
    const bool voicesKept = AVDexedLibrary::readBank(file, onDisk) && AVDexedLibrary::bankFingerprint(onDisk) == fingerprint;

    // A typed name keeps accented letters readable in the 10 DX7 characters.
    processor.setAVBankProgram(5);
    const bool renamed = processor.saveAVSoundToBank(juce::String(juce::CharPointer_UTF8("Canci\xc3\xb3n \xc3\x91u")))
                             == DexedAudioProcessor::AVBankSaveResult::saved
        && processor.programNames[5].trimEnd() == "Cancion Nu"
        && processor.activeProgram.getName().trimEnd() == "Cancion Nu"
        && !processor.isAVPresetModified();

    return expect(written && inherited && savedCopy && twinKept && copyDry && explicitEntry,
                  "a duplicate voice saved at the defaults stays dry and its twin keeps its settings")
        && expect(staleNotApplied && savedSlot3 && movedKept && slot3Own,
                  "saving a slot keeps the settings of a voice that was moved away from it")
        && expect(voicesKept, "saving settings, or the name the voice already has, keeps the voice bytes")
        && expect(renamed, "a typed name folds accents into the DX7 name");
}

bool testLegacyRestoreInstallsItsBank()
{
    AVScratchLibrary library;
    const auto bank = makeDistinctBank(8);
    AVDexedLibrary::BankSettings settings;
    settings[2] = { AVDexedLibrary::voiceHash(bank, 2), { { "reverbAmount", 0.5f } } };
    const auto file = library.root.getChildFile("Viejos").getChildFile("Legacy.syx");
    const bool written = writeBankFile(file, bank, settings);

    // A Dexed 1.0 state that points into 0.1.0's Cartridges/ folder.
    juce::XmlElement root("dexedState");
    root.setAttribute("currentProgram", 2);
    root.setAttribute("activeFileCartridge",
                      library.root.getChildFile("Cartridges").getChildFile("Viejos").getChildFile("Legacy.syx").getFullPathName());
    auto* blob = root.createNewChildElement("dexedBlob");
    juce::NamedValueSet attributes;
    attributes.set("program", juce::var(Program::initialProgram().getRawData(), Program::PROGRAM_SIZE));
    attributes.copyToXmlAttributes(*blob);
    juce::MemoryBlock legacy;
    juce::AudioProcessor::copyXmlToBinary(root, legacy);

    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    processor.setStateInformation(legacy.getData(), static_cast<int>(legacy.getSize()));
    // The bank's voice 2; the host's one program is the voice that plays (the state's own).
    const bool installed = processor.activeFileCartridge == file && processor.getAVBankProgram() == 2
        && processor.programNames[2].trimEnd() == "B8 S02" && processor.getProgramName(0) == processor.getAVPresetName()
        && near(parameterValue(processor, IDs::reverbAmount.name), 0.0f, 0.0001f);
    processor.setAVBankProgram(2);
    const bool guarded = near(parameterValue(processor, IDs::reverbAmount.name), 0.0f, 0.0001f);
    processor.setAVBankProgram(2);
    const bool appliedLater = near(parameterValue(processor, IDs::reverbAmount.name), 0.5f);

    return expect(written && installed, "a legacy state remaps its bank into the library and installs it without applying settings")
        && expect(guarded && appliedLater, "a legacy restore arms the restore guard too");
}

bool testSysexBankClearsFileContext()
{
    AVScratchLibrary library;
    const auto bank = makeDistinctBank(6);
    AVDexedLibrary::BankSettings settings;
    settings[0] = { AVDexedLibrary::voiceHash(bank, 0), { { "masterVolume", 0.5f } } };
    const auto file = library.root.getChildFile("Mis bancos").getChildFile("Antes.syx");
    const bool written = writeBankFile(file, bank, settings);

    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    processor.setActiveBank(bank, file);
    processor.setAVBankProgram(0);
    const bool before = near(parameterValue(processor, IDs::masterVolume.name), 0.5f);
    processor.setAVBankProgram(1);

    // The same voices arrive as a 32-voice dump: they have no file now, so the
    // old file's settings must not come back with them.
    processor.setRateAndBufferSizeDetails(48000.0, 64);
    processor.prepareToPlay(48000.0, 64);
    Cartridge dump = bank;
    juce::AudioBuffer<float> audio(2, 64); audio.clear();
    juce::MidiBuffer events;
    events.addEvent(juce::MidiMessage(dump.getVoiceSysex(), SYSEX_SIZE), 0);
    processor.processBlock(audio, events);
    const bool deferred = processor.activeFileCartridge == file;
    processor.flushAVPendingSysExForTesting();
    processor.releaseResources();
    const bool cleared = processor.activeFileCartridge == juce::File() && !processor.hasExternalCartridgeContext()
        && processor.getAVBankProgram() == 0 && processor.programNames[0].trimEnd() == "B6 S00";
    const bool defaults = hasDefaultSoundSettings(processor) && !processor.canSaveAVSoundToBank();

    return expect(written && before && deferred, "the file's settings apply before the dump, which is handled outside render")
        && expect(cleared && defaults, "a SysEx bank clears the file context and its settings: defaults, and Guardar goes to Save As");
}

//==============================================================================
// Default CC assignments, and the MIDI Learn of the previous / next sound arrows.

bool testDefaultWheelClosesTheFilter()
{
    DexedAudioProcessor fresh(DexedAudioProcessor::TestMode{});
    const auto defaults = fresh.midiCCMapper->createAVMappingsSnapshot();
    const auto wheel = defaults.getChild(0);
    const bool defaultPresent = defaults.getNumChildren() == 1
        && static_cast<int>(wheel.getProperty(IDs::midiCC)) == 1
        && wheel.getProperty(IDs::midiParameterName).toString() == IDs::cutoff.name
        && static_cast<bool>(wheel.getProperty(IDs::midiInvert, false))
        && fresh.midiCCMapper->getMapping(IDs::cutoff.name) == 1 && fresh.midiCCMapper->isAVInverted(1)
        && fresh.getAVPresetStepCC(DexedAudioProcessor::AVPresetStep::previous) == -1
        && fresh.getAVPresetStepCC(DexedAudioProcessor::AVPresetStep::next) == -1
        && !fresh.isAVPresetModified();

    // The wheel closes the filter as it rises, and still drives the DX7 modulation.
    fresh.setRateAndBufferSizeDetails(48000.0, 64);
    fresh.prepareToPlay(48000.0, 64);
    juce::AudioBuffer<float> audio(2, 64); audio.clear();
    juce::MidiBuffer events;
    events.addEvent(juce::MidiMessage::controllerEvent(1, 1, 127), 0);
    fresh.processBlock(audio, events);
    const bool wheelUp = near(parameterValue(fresh, IDs::cutoff.name), 0.0f, 0.0001f)
        && fresh.controllers.modwheel_cc == 127 && fresh.getAVKeyboardMod() == 127;
    events.clear(); audio.clear();
    events.addEvent(juce::MidiMessage::controllerEvent(1, 1, 0), 0);
    fresh.processBlock(audio, events);
    const bool wheelDown = near(parameterValue(fresh, IDs::cutoff.name), 1.0f, 0.0001f)
        && fresh.controllers.modwheel_cc == 0 && !fresh.isAVPresetModified();

    // A session saved with the wheel up reopens unmodified: the wheel's
    // Cutoff is played, the sound's own value goes with it. A real edit
    // made before still counts.
    events.clear(); audio.clear();
    events.addEvent(juce::MidiMessage::controllerEvent(1, 1, 100), 0);
    fresh.processBlock(audio, events);
    juce::MemoryBlock wheelSession;
    fresh.getStateInformation(wheelSession);
    DexedAudioProcessor reopened(DexedAudioProcessor::TestMode{});
    const bool wheelReopens = reopened.loadAVStateInformation(wheelSession.getData(), static_cast<int>(wheelSession.getSize()))
        && near(parameterValue(reopened, IDs::cutoff.name), parameterValue(fresh, IDs::cutoff.name), 0.0001f)
        && !reopened.isAVPresetModified();
    juce::MemoryBlock wheelAgain;
    reopened.getStateInformation(wheelAgain);
    DexedAudioProcessor reopenedTwice(DexedAudioProcessor::TestMode{});
    const bool wheelReopensTwice = reopenedTwice.loadAVStateInformation(wheelAgain.getData(), static_cast<int>(wheelAgain.getSize()))
        && !reopenedTwice.isAVPresetModified();
    const auto wheelSound = fresh.createAVNativePresetState("Rueda", "av-wheel");
    const auto wheelSoundXml = juce::AudioProcessor::getXmlFromBinary(wheelSound.getData(), static_cast<int>(wheelSound.getSize()));
    const bool soundFileClean = wheelSoundXml != nullptr && wheelSoundXml->getChildByName("session") != nullptr
        && wheelSoundXml->getChildByName("session")->getChildByName("avCCWrites") == nullptr;
    if (auto* resonance = fresh.parameters.getParameter(IDs::resonance.name))
        resonance->setValueNotifyingHost(resonance->getValue() > 0.5f ? 0.1f : 0.9f);
    juce::MemoryBlock editedSession;
    fresh.getStateInformation(editedSession);
    DexedAudioProcessor reopenedEdited(DexedAudioProcessor::TestMode{});
    const bool editStillCounts = reopenedEdited.loadAVStateInformation(editedSession.getData(), static_cast<int>(editedSession.getSize()))
        && reopenedEdited.isAVPresetModified();
    fresh.releaseResources();

    // Saved like any mapping (here into an instance that had removed it)...
    DexedAudioProcessor restored(DexedAudioProcessor::TestMode{});
    restored.midiCCMapper->removeMapping(1);
    juce::MemoryBlock state;
    fresh.getStateInformation(state);
    const bool roundTrip = restored.loadAVStateInformation(state.getData(), static_cast<int>(state.getSize()))
        && restored.midiCCMapper->getAVMappingParameterId(1) == IDs::cutoff.name
        && restored.midiCCMapper->isAVInverted(1);

    // ...and a normal one: it can be flipped, moved and removed, and a session
    // whose list was emptied stays empty (the default is only a new instance's).
    restored.midiCCMapper->setAVInverted(1, false);
    const bool flipped = !restored.midiCCMapper->isAVInverted(1);
    const bool moved = restored.midiCCMapper->moveAVMapping(1, 74)
        && restored.midiCCMapper->getMapping(IDs::cutoff.name) == 74;
    fresh.midiCCMapper->removeMapping(1);
    juce::MemoryBlock emptied;
    fresh.getStateInformation(emptied);
    DexedAudioProcessor another(DexedAudioProcessor::TestMode{});
    const bool emptyStaysEmpty = another.loadAVStateInformation(emptied.getData(), static_cast<int>(emptied.getSize()))
        && another.midiCCMapper->getMapping(IDs::cutoff.name) == -1
        && another.midiCCMapper->createAVMappingsSnapshot().getNumChildren() == 0;
    juce::MemoryBlock emptiedAgain;
    another.getStateInformation(emptiedAgain);
    DexedAudioProcessor third(DexedAudioProcessor::TestMode{});
    const bool staysEmptyTwice = third.loadAVStateInformation(emptiedAgain.getData(), static_cast<int>(emptiedAgain.getSize()))
        && third.midiCCMapper->createAVMappingsSnapshot().getNumChildren() == 0;

    // Back to the default assignments: CC1 → Cutoff (inverted) again.
    third.midiCCMapper->restoreAVDefaultMappings();
    const bool resetRestores = third.midiCCMapper->getMapping(IDs::cutoff.name) == 1
        && third.midiCCMapper->isAVInverted(1)
        && third.midiCCMapper->createAVMappingsSnapshot().getNumChildren() == 1;

    return expect(defaultPresent, "a new instance maps CC1 to Cutoff, inverted, as a normal mapping, and has no arrow CCs")
        && expect(wheelUp && wheelDown, "CC1 closes the filter as it rises and still drives the DX7 mod wheel")
        && expect(wheelReopens && wheelReopensTwice, "a session saved with the wheel up reopens unmodified")
        && expect(soundFileClean, "a sound file does not carry what the wheel moved")
        && expect(editStillCounts, "an edit saved in a session still reopens modified")
        && expect(roundTrip, "the default mapping survives a session round trip")
        && expect(flipped && moved, "the default mapping can be flipped and moved like any other")
        && expect(emptyStaysEmpty && staysEmptyTwice, "a session whose mappings were removed stays empty after restore")
        && expect(resetRestores, "restoring the default assignments brings CC1 → Cutoff back");
}

bool testPresetStepLearnAndPresses()
{
    using Step = DexedAudioProcessor::AVPresetStep;
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    auto& handler = *processor.midiCCMapper;

    // Learned in render, visible at once, reported once the UI stage commits it.
    processor.startAVPresetStepLearn(Step::next);
    handler.processControlChange(20, 127);
    const bool pendingVisible = processor.getAVPresetStepCC(Step::next) == 20
        && processor.consumeAVPresetStepLearned() == -1;
    handler.commitAVPresetStepLearn();
    const bool learned = processor.consumeAVPresetStepLearned() == 1
        && processor.consumeAVPresetStepLearned() == -1
        && processor.consumeAVPresetSteps() == 0;  // the press that taught it is not a step

    // A press is a rise from below 64 to 64 or above.
    for (const int value : { 0, 127, 127, 90, 0, 63, 64, 10, 100 })
        handler.processControlChange(20, value);
    const bool risingEdges = processor.consumeAVPresetSteps() == 3 && processor.consumeAVPresetSteps() == 0;

    processor.startAVPresetStepLearn(Step::previous);
    handler.processControlChange(21, 127);
    handler.commitAVPresetStepLearn();
    for (const int value : { 0, 127, 0, 127 })
        handler.processControlChange(21, value);
    handler.processControlChange(20, 0);
    handler.processControlChange(20, 127);
    const bool bothArrows = processor.consumeAVPresetStepLearned() == 0 && processor.consumeAVPresetSteps() == -1
        && processor.getAVPresetStepCC(Step::previous) == 21 && processor.getAVPresetStepCC(Step::next) == 20;

    // One CC drives one thing: an arrow takes CC1 from the Cutoff (which no
    // longer moves), and leaves the CC it had.
    processor.startAVPresetStepLearn(Step::next);
    handler.processControlChange(1, 127);
    const bool pendingHidesMapping = handler.getAVMappingParameterId(1).isEmpty()
        && handler.createAVMappingsSnapshot().getNumChildren() == 0;
    handler.commitAVPresetStepLearn();
    processor.consumeAVPresetStepLearned();
    handler.processControlChange(1, 0);
    handler.processControlChange(1, 127);
    handler.processControlChange(20, 0);
    handler.processControlChange(20, 127);
    const bool tookFromParameter = processor.getAVPresetStepCC(Step::next) == 1
        && handler.getMapping(IDs::cutoff.name) == -1
        && near(parameterValue(processor, IDs::cutoff.name), 1.0f, 0.0001f)
        && processor.consumeAVPresetSteps() == 1;

    // A parameter Learn cancels an arrow Learn and takes the arrow's CC.
    processor.startAVPresetStepLearn(Step::previous);
    handler.startMapping(processor.parameters.getParameter(IDs::pan.name), {});
    handler.processControlChange(21, 127);
    const bool parameterPendingWins = processor.getAVPresetStepCC(Step::previous) == -1
        && handler.getMapping(IDs::pan.name) == 21;
    handler.commitAVPendingMapping();
    handler.commitAVPresetStepLearn();
    const bool parameterTookArrowCC = processor.consumeAVPresetStepLearned() == -1
        && processor.getAVPresetStepCC(Step::previous) == -1 && handler.getMapping(IDs::pan.name) == 21
        && !handler.moveAVMapping(21, 1) && handler.getMapping(IDs::pan.name) == 21;  // CC1 is the next arrow's

    // An arrow Learn cancels a parameter Learn.
    handler.startMapping(processor.parameters.getParameter(IDs::reverbAmount.name), {});
    processor.startAVPresetStepLearn(Step::previous);
    handler.processControlChange(22, 127);
    handler.commitAVPendingMapping();
    handler.commitAVPresetStepLearn();
    const bool arrowCancelsParameter = handler.getMapping(IDs::reverbAmount.name) == -1
        && processor.getAVPresetStepCC(Step::previous) == 22 && processor.consumeAVPresetStepLearned() == 0;

    // Cancelling drops a CC render took but the UI stage had not committed;
    // clearing leaves the arrow without CC.
    processor.startAVPresetStepLearn(Step::previous);
    handler.processControlChange(23, 127);
    processor.cancelAVPresetStepLearn();
    handler.commitAVPresetStepLearn();
    const bool cancelled = processor.getAVPresetStepCC(Step::previous) == 22
        && processor.consumeAVPresetStepLearned() == -1;
    processor.clearAVPresetStepCC(Step::previous);
    handler.processControlChange(22, 0);
    handler.processControlChange(22, 127);
    const bool cleared = processor.getAVPresetStepCC(Step::previous) == -1 && processor.consumeAVPresetSteps() == 0;

    return expect(pendingVisible && learned, "an arrow Learn is visible from render and reported once after the UI commit")
        && expect(risingEdges && bothArrows, "a learned CC steps once per rise from below 64 to 64 or above, previous -1 and next +1")
        && expect(pendingHidesMapping && tookFromParameter, "an arrow takes its CC from a parameter mapping and from its old CC")
        && expect(parameterPendingWins && parameterTookArrowCC, "a parameter Learn cancels an arrow Learn and takes the arrow's CC")
        && expect(arrowCancelsParameter, "an arrow Learn cancels a parameter Learn")
        && expect(cancelled && cleared, "cancel drops an uncommitted arrow CC and clear removes it");
}

bool testPresetStepPersistenceAndBankStepping()
{
    using Step = DexedAudioProcessor::AVPresetStep;
    DexedAudioProcessor source(DexedAudioProcessor::TestMode{});
    source.startAVPresetStepLearn(Step::previous);
    source.midiCCMapper->processControlChange(116, 127);
    source.midiCCMapper->commitAVPresetStepLearn();
    source.startAVPresetStepLearn(Step::next);
    source.midiCCMapper->processControlChange(117, 127);
    // Saved before the UI stage committed it, like a parameter Learn.
    juce::MemoryBlock state;
    source.getStateInformation(state);
    DexedAudioProcessor restored(DexedAudioProcessor::TestMode{});
    const bool roundTrip = restored.loadAVStateInformation(state.getData(), static_cast<int>(state.getSize()))
        && restored.getAVPresetStepCC(Step::previous) == 116 && restored.getAVPresetStepCC(Step::next) == 117
        && restored.midiCCMapper->getMapping(IDs::cutoff.name) == 1;

    // A session from before the arrows had none; an unusable CC, or one the
    // session's own mappings use, is dropped without refusing the session.
    auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    auto tree = juce::ValueTree::fromXml(*xml);
    auto mappings = tree.getChildWithName(IDs::profile).getChildWithName(IDs::midiCCMappings);
    const bool stored = static_cast<int>(mappings.getProperty(IDs::avPresetPreviousCC, -1)) == 116
        && static_cast<int>(mappings.getProperty(IDs::avPresetNextCC, -1)) == 117;
    mappings.removeProperty(IDs::avPresetPreviousCC, nullptr);
    mappings.removeProperty(IDs::avPresetNextCC, nullptr);
    const auto older = encodeTree(tree);
    const bool olderHasNone = restored.loadAVStateInformation(older.getData(), static_cast<int>(older.getSize()))
        && restored.getAVPresetStepCC(Step::previous) == -1 && restored.getAVPresetStepCC(Step::next) == -1;
    mappings.setProperty(IDs::avPresetPreviousCC, "300", nullptr);
    mappings.setProperty(IDs::avPresetNextCC, 1, nullptr);
    const auto odd = encodeTree(tree);
    const bool oddDropped = restored.loadAVStateInformation(odd.getData(), static_cast<int>(odd.getSize()))
        && restored.getAVPresetStepCC(Step::previous) == -1 && restored.getAVPresetStepCC(Step::next) == -1
        && restored.midiCCMapper->getMapping(IDs::cutoff.name) == 1;

    // A sound file neither carries the arrow CCs nor takes them away.
    restored.loadAVStateInformation(state.getData(), static_cast<int>(state.getSize()));
    const auto native = restored.createAVNativePresetState("Sonido", "av-step-id");
    auto nativeXml = juce::AudioProcessor::getXmlFromBinary(native.getData(), static_cast<int>(native.getSize()));
    const auto nativeMappings = juce::ValueTree::fromXml(*nativeXml).getChildWithName(IDs::profile)
        .getChildWithName(IDs::midiCCMappings);
    DexedAudioProcessor player(DexedAudioProcessor::TestMode{});
    player.startAVPresetStepLearn(Step::previous);
    player.midiCCMapper->processControlChange(30, 127);
    player.midiCCMapper->commitAVPresetStepLearn();
    const bool nativeKeeps = !nativeMappings.hasProperty(IDs::avPresetPreviousCC)
        && !nativeMappings.hasProperty(IDs::avPresetNextCC) && nativeMappings.getNumChildren() == 1
        && player.loadAVNativePreset(native.getData(), static_cast<int>(native.getSize()))
        && player.getAVPresetStepCC(Step::previous) == 30 && player.getAVPresetStepCC(Step::next) == -1;

    // With no editor open, the processor's timer moves through the current bank.
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    processor.setActiveBank(makeDistinctBank(7), juce::File());
    processor.setAVBankProgram(0);
    for (const auto& [step, cc] : { std::pair { Step::previous, 116 }, std::pair { Step::next, 117 } }) {
        processor.startAVPresetStepLearn(step);
        processor.midiCCMapper->processControlChange(cc, 127);
        processor.midiCCMapper->commitAVPresetStepLearn();
    }
    processor.setRateAndBufferSizeDetails(48000.0, 64);
    processor.prepareToPlay(48000.0, 64);
    juce::AudioBuffer<float> audio(2, 64); audio.clear();
    juce::MidiBuffer events;
    int position = 0;
    for (const int value : { 0, 127, 0, 127 })
        events.addEvent(juce::MidiMessage::controllerEvent(1, 117, value), position++);
    processor.processBlock(audio, events);
    // With no editor the press steps right in render, at its place in the
    // timeline (as a Program Change does), so an offline bounce matches.
    const bool deferredToTimer = processor.getAVBankProgram() == 2;
    processor.runAVTimerTickForTesting();
    const bool steppedForward = processor.getAVBankProgram() == 2
        && processor.programNames[2].trimEnd() == "B7 S02" && !processor.isAVPresetModified();
    events.clear(); audio.clear(); position = 0;
    for (const int value : { 0, 127, 0, 127, 0, 127 })
        events.addEvent(juce::MidiMessage::controllerEvent(1, 116, value), position++);
    processor.processBlock(audio, events);
    processor.runAVTimerTickForTesting();
    const bool wrapped = processor.getAVBankProgram() == 31 && processor.consumeAVPresetSteps() == 0;
    processor.releaseResources();

    return expect(roundTrip && stored, "both arrow CCs are saved with the MIDI assignments, even before the UI commit")
        && expect(olderHasNone && oddDropped, "an older session restores no arrow CCs; unusable or conflicting ones are dropped")
        && expect(nativeKeeps, "a sound file neither stores nor replaces the arrow CCs")
        && expect(deferredToTimer && steppedForward && wrapped, "without an editor each press steps through the bank in render, wrapping 0..31");
}

bool testOlderListsGetTheWheelDefault()
{
    // A list saved before AV Dexed had default assignments (no version): an
    // older DAW session keeps it exactly (CC1 recorded there for the DX7 vibrato
    // must not start closing the filter); an older sound file gets CC1 →
    // Cutoff where CC1 and the Cutoff are free, so loading it does not take the
    // default away from the instance. A list saved now keeps what it says.
    DexedAudioProcessor source(DexedAudioProcessor::TestMode{});
    juce::MemoryBlock state;
    source.getStateInformation(state);
    const auto older = [&state](const std::vector<std::pair<int, juce::String>>& entries, bool soundFile) {
        auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
        auto tree = juce::ValueTree::fromXml(*xml);
        auto mappings = tree.getChildWithName(IDs::profile).getChildWithName(IDs::midiCCMappings);
        mappings.removeAllChildren(nullptr);
        mappings.removeAllProperties(nullptr);
        for (const auto& [cc, id] : entries) {
            juce::ValueTree entry(IDs::midiCCMapping);
            entry.setProperty(IDs::midiCC, cc, nullptr);
            entry.setProperty(IDs::midiParameterName, id, nullptr);
            mappings.addChild(entry, -1, nullptr);
        }
        // Older builds hashed the sound with the previous recipe.
        tree.getChildWithName("session").setProperty("avPresetBaselineFormat", 2, nullptr);
        if (soundFile) tree.setProperty("nativePresetVersion", 1, nullptr);
        return encodeTree(tree);
    };

    DexedAudioProcessor restored(DexedAudioProcessor::TestMode{});
    restored.midiCCMapper->removeMapping(1);
    const auto empty = older({}, false);
    const bool emptyGetsIt = restored.loadAVStateInformation(empty.getData(), static_cast<int>(empty.getSize()))
        && restored.midiCCMapper->getMapping(IDs::cutoff.name) == -1 && restored.midiCCMapper->getAVMappingParameterId(1).isEmpty()
        && !restored.isAVPresetModified();
    const auto withReverb = older({ { 20, IDs::reverbAmount.name } }, false);
    const bool joinsOthers = restored.loadAVStateInformation(withReverb.getData(), static_cast<int>(withReverb.getSize()))
        && restored.midiCCMapper->getMapping(IDs::reverbAmount.name) == 20
        && restored.midiCCMapper->getMapping(IDs::cutoff.name) == -1;
    const auto wheelOnPan = older({ { 1, IDs::pan.name } }, false);
    const bool wheelTaken = restored.loadAVStateInformation(wheelOnPan.getData(), static_cast<int>(wheelOnPan.getSize()))
        && restored.midiCCMapper->getMapping(IDs::pan.name) == 1
        && restored.midiCCMapper->getMapping(IDs::cutoff.name) == -1;
    const auto cutoffOnKnob = older({ { 74, IDs::cutoff.name } }, false);
    const bool cutoffTaken = restored.loadAVStateInformation(cutoffOnKnob.getData(), static_cast<int>(cutoffOnKnob.getSize()))
        && restored.midiCCMapper->getMapping(IDs::cutoff.name) == 74 && !restored.midiCCMapper->isAVInverted(74)
        && restored.midiCCMapper->getAVMappingParameterId(1).isEmpty();

    DexedAudioProcessor player(DexedAudioProcessor::TestMode{});
    player.midiCCMapper->removeMapping(1);
    const auto oldSound = older({}, true);
    const bool oldSoundGetsIt = player.loadAVNativePreset(oldSound.getData(), static_cast<int>(oldSound.getSize()))
        && player.midiCCMapper->getMapping(IDs::cutoff.name) == 1;
    source.midiCCMapper->removeMapping(1);
    const auto emptiedSound = source.createAVNativePresetState("Sin rueda", "av-no-wheel");
    const bool chosenEmptyKept = player.loadAVNativePreset(emptiedSound.getData(), static_cast<int>(emptiedSound.getSize()))
        && player.midiCCMapper->getMapping(IDs::cutoff.name) == -1
        && player.midiCCMapper->createAVMappingsSnapshot().getNumChildren() == 0;

    return expect(emptyGetsIt && joinsOthers, "an older DAW session keeps its own list (no CC1 → Cutoff added) and is not marked modified")
        && expect(wheelTaken && cutoffTaken, "an older list that already uses CC1 or the Cutoff keeps its own choice")
        && expect(oldSoundGetsIt && chosenEmptyKept, "an older sound file gets the default; one saved empty now stays empty");
}

bool testWheelIsPlayedAndHeldPedalsDoNotRepeat()
{
    using Step = DexedAudioProcessor::AVPresetStep;
    // The mod wheel is played: what it moves (CC1 → Cutoff) is no edit of the
    // sound, or every wheel gesture would ask to save on the next sound change.
    DexedAudioProcessor processor(DexedAudioProcessor::TestMode{});
    auto& handler = *processor.midiCCMapper;
    handler.processControlChange(1, 100);
    const bool wheelIsPlayed = parameterValue(processor, IDs::cutoff.name) < 0.5f && !processor.isAVPresetModified();
    setParameterValue(processor, IDs::masterVolume.name, 0.4f);
    const bool otherEditsCount = processor.isAVPresetModified();
    // On another control the Cutoff is part of the sound again.
    const bool moved = handler.moveAVMapping(1, 74);
    processor.captureAVPresetBaseline();
    handler.processControlChange(74, 20);
    const bool knobCounts = moved && processor.isAVPresetModified();

    // A learned pedal still held down after the step that loaded a sound file
    // keeps moving, but is no new press; the next press is.
    DexedAudioProcessor player(DexedAudioProcessor::TestMode{});
    const auto sound = player.createAVNativePresetState("Pedal", "av-pedal");
    player.startAVPresetStepLearn(Step::next);
    player.midiCCMapper->processControlChange(11, 0);
    player.midiCCMapper->commitAVPresetStepLearn();
    player.midiCCMapper->processControlChange(11, 70);
    const bool firstStep = player.consumeAVPresetSteps() == 1;
    const bool loaded = player.loadAVNativePreset(sound.getData(), static_cast<int>(sound.getSize()));
    for (const int value : { 75, 90, 127 })
        player.midiCCMapper->processControlChange(11, value);
    const bool noRepeat = loaded && player.consumeAVPresetSteps() == 0 && player.getAVPresetStepCC(Step::next) == 11;
    player.midiCCMapper->processControlChange(11, 10);
    player.midiCCMapper->processControlChange(11, 100);
    const bool nextPress = player.consumeAVPresetSteps() == 1;

    return expect(wheelIsPlayed && otherEditsCount && knobCounts,
                  "the mod wheel's parameter does not mark the sound modified; other edits and other controls do")
        && expect(firstStep && noRepeat && nextPress, "a pedal held across a sound-file load does not step again");
}

} // namespace

bool runAVDexedStateTests()
{
    return testCompleteVoiceParameterMapping()
        && testStateRoundTripAndInvalidState()
        && testVersion100ParameterMigration()
        && testIndependentProcessorInstances()
        && testOldSessionsResetMissingEffects()
        && testNativeContextAndDirtyRoundTrip()
        && testSemanticInvalidStatePreservesSound()
        && testSlotBankAndSysexRoundTrip()
        && testOutOfRangeBankValuesAreClamped()
        && testLegacyBlobMigrationAndInvalidBlob()
        && testAudioRoutingMidiAndSampleRates()
        && testMidiLearnCC127AndSessionRestore()
        && testDeferredSysexWithoutEditorAndOutput()
        && testOperatorGlowFollowsAudibleCarriers()
        && testSoundSettingsFollowTheirVoice()
        && testRestoreKeepsSessionValues()
        && testRestoreGuardOnlyForRestoredBank()
        && testRestoreKeepsSaveTarget()
        && testSaveSoundToBank()
        && testSaveNeedsSaveAs()
        && testSaveKeepsOtherSlotsAsTheyWere()
        && testLegacyRestoreInstallsItsBank()
        && testSysexBankClearsFileContext()
        && testDefaultWheelClosesTheFilter()
        && testPresetStepLearnAndPresses()
        && testPresetStepPersistenceAndBankStepping()
        && testOlderListsGetTheWheelDefault()
        && testWheelIsPlayedAndHeldPedalsDoNotRepeat();
}
