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
        && restored.getCurrentProgram() == 7
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
        && restored.getCurrentProgram() == 7;

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
        && first.getCurrentProgram() == 2;
    const bool secondUnaffected = std::abs(parameterValue(second, IDs::masterVolume.name) - 0.81f) < 0.002f
        && std::abs(parameterValue(second, IDs::pan.name) - 0.76f) < 0.002f
        && std::abs(parameterValue(second, IDs::reverbAmount.name) - 0.09f) < 0.002f
        && second.activeProgram.getName() == "INSTANCE B"
        && second.getCurrentProgram() == 9;

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
    const bool preservedVoice = loaded && processor.getCurrentProgram() == 12
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
                  && processor.getCurrentProgram() == 12,
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
        && testLegacyBlobMigrationAndInvalidBlob()
        && testAudioRoutingMidiAndSampleRates()
        && testMidiLearnCC127AndSessionRestore()
        && testDeferredSysexWithoutEditorAndOutput()
        && testOperatorGlowFollowsAudibleCarriers();
}
