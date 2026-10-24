#include "PluginProcessor.h"
#include "parameter/Model.h"
#include "StateLegacy.h"
#include "midi/MidiCCHandler.h"
#include <cstdlib>
#include <set>

namespace {
constexpr int currentStateSchema = 2;
constexpr auto sessionNode = "session";
constexpr auto voiceProperty = "voice";
constexpr auto cartridgeProperty = "cartridge";

bool decodeFixedBlock(const juce::String& encoded, size_t expectedSize, juce::MemoryBlock& decoded) {
    if (encoded.isEmpty() || !decoded.fromBase64Encoding(encoded) || decoded.getSize() != expectedSize)
        return false;
    return true;
}

bool avPrepareParameterState(DexedApvts& parameters, juce::ValueTree& state) {
    std::set<juce::String> ids;
    for (const auto& child : state) {
        const auto id = child.getProperty(IDs::id).toString();
        auto* parameter = parameters.getParameter(id);
        if (id.isEmpty() || parameter == nullptr || !ids.insert(id).second || !child.hasProperty(IDs::value))
            return false;
        const auto text = child.getProperty(IDs::value).toString().trim();
        char* end = nullptr;
        const auto value = std::strtod(text.toRawUTF8(), &end);
        const auto& range = parameter->getNormalisableRange();
        if (end == text.toRawUTF8() || *end != '\0' || !std::isfinite(value)
            || value < range.start - 0.0001 || value > range.end + 0.0001)
            return false;
    }
    if (ids.empty())
        return false;

    // APVTS otherwise retains the last live value for a missing parameter.
    // New controls in old sessions must start from product defaults, even if
    // another sound had enabled effects immediately before the restore.
    bool newParameters = false;
    for (const auto* abstractParameter : parameters.processor.getParameters()) {
        const auto* parameter = dynamic_cast<const juce::RangedAudioParameter*>(abstractParameter);
        if (parameter == nullptr)
            continue;
        newParameters |= parameter->paramID == IDs::masterVolume.name;
        if (!newParameters || ids.count(parameter->paramID) != 0)
            continue;
        juce::ValueTree child("PARAM");
        child.setProperty(IDs::id, parameter->paramID, nullptr);
        child.setProperty(IDs::value, parameter->convertFrom0to1(parameter->getDefaultValue()), nullptr);
        state.addChild(child, -1, nullptr);
    }
    return true;
}

bool avValidateTuningData(const juce::String& scl, const juce::String& kbm) {
    try {
        if (scl.isNotEmpty() && kbm.isNotEmpty()) {
            Tunings::Tuning tuning(Tunings::parseSCLData(scl.toStdString()), Tunings::parseKBMData(kbm.toStdString()));
        } else if (scl.isNotEmpty()) {
            Tunings::Tuning tuning(Tunings::parseSCLData(scl.toStdString()));
        } else if (kbm.isNotEmpty()) {
            Tunings::Tuning tuning(Tunings::parseKBMData(kbm.toStdString()));
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
}

//==============================================================================
void DexedAudioProcessor::getStateInformation(MemoryBlock& destData) {
    const juce::ScopedLock avStateLock(getCallbackLock());
    auto state = parameters.copyState();
    juce::ValueTree storage { IDs::root };
    storage.setProperty(IDs::version, DexedApvts::MODEL_VERSION, nullptr);
    storage.setProperty("schemaVersion", currentStateSchema, nullptr);
    storage.addChild(state, -1, nullptr);

    auto profile = parameters.rootVt.getChildWithName(IDs::profile);
    if (profile.isValid()) {
        auto snapshotProfile = profile.createCopy();
        snapshotProfile.removeChild(snapshotProfile.getChildWithName(IDs::midiCCMappings), nullptr);
        snapshotProfile.addChild(midiCCMapper->createAVMappingsSnapshot(), -1, nullptr);
        storage.addChild(snapshotProfile, -1, nullptr);
    }

    juce::ValueTree tuningVt("tuning");
    tuningVt.setProperty("scl", juce::String(currentSCLData), nullptr);
    tuningVt.setProperty("kbm", juce::String(currentKBMData), nullptr);
    storage.addChild(tuningVt, -1, nullptr);

    juce::ValueTree session(sessionNode);
    session.setProperty("currentProgram", currentProgram, nullptr);
    session.setProperty("sourcePath", activeFileCartridge.getFullPathName(), nullptr);
    session.setProperty("avPresetName", avPresetContextCurrent.load(std::memory_order_relaxed) ? avPresetName : juce::String(), nullptr);
    session.setProperty("avPresetId", getAVPresetId(), nullptr);
    session.setProperty("avKnownPresetOrigin", hasAVKnownPresetOrigin(), nullptr);
    // Whether the sound still is its bank slot's own (perhaps edited), or an
    // Init or single SysEx voice that "Guardar" must not write over the slot.
    session.setProperty("avSoundFromBankSlot", avSoundFromBankSlot.load(std::memory_order_relaxed), nullptr);
    session.setProperty("avPresetPath", getAVPresetPath().getFullPathName(), nullptr);
    session.setProperty("avPresetBaseline", juce::String::toHexString(static_cast<juce::int64>(avPresetBaseline.load(std::memory_order_relaxed))), nullptr);
    session.setProperty("avPresetBaselineFormat", avPresetBaselineFormat, nullptr);

    juce::MemoryBlock voiceData(activeProgram.getRawData(), Program::PROGRAM_SIZE + 1);
    session.setProperty(voiceProperty, voiceData.toBase64Encoding(), nullptr);

    if (currentCart.containsData()) {
        const auto* sysex = currentCart.getVoiceSysex();
        juce::MemoryBlock cartridgeData(sysex, SYSEX_SIZE);
        session.setProperty(cartridgeProperty, cartridgeData.toBase64Encoding(), nullptr);
        if (const auto* amem = currentCart.getAmem())   // the DX7II additional data of its voices
            session.setProperty("cartridgeAmem", juce::MemoryBlock(amem, 1120).toBase64Encoding(), nullptr);
    }

    storage.addChild(session, -1, nullptr);
    storage.addChild(createAVLayersState(), -1, nullptr);

    if (auto xml = storage.createXml())
        copyXmlToBinary(*xml, destData);
}

void DexedAudioProcessor::setStateInformation(const void* source, int sizeInBytes) {
    // A host restore: the session's values win over the per-sound settings,
    // also when the host re-selects the restored program right afterwards.
    // (Whether "Guardar" may write into the bank slot comes from the session:
    // loadAVStateInformation() restores it.)
    if (loadAVStateInformation(source, sizeInBytes))
        armAVRestoreGuard();
}

bool DexedAudioProcessor::loadAVNativePreset(const void* source, int sizeInBytes) {
    // Saved by "Guardar capas": the file's own layers replace all of them.
    juce::ValueTree layers;
    if (source != nullptr && sizeInBytes > 0 && sizeInBytes <= 32 * 1024 * 1024)
        if (const auto xml = getXmlFromBinary(source, sizeInBytes))
            if (const auto* child = xml->getChildByName("avLayers"))
                layers = juce::ValueTree::fromXml(*child);
    const float output = avOutputParameter != nullptr ? avOutputParameter->getValue() : 0.0f;
    const float tempo = avDelayTempoParameter != nullptr ? avDelayTempoParameter->getValue() : 0.0f;
    {
        // The sound arrows' CCs are the instrument's too: walking onto a sound
        // file must not take them away, nor re-arm a pedal still held down
        // (its own mappings come with it, as always; one on an arrow's CC gives
        // way to the arrow). See MidiCCHandler::restoreAVMappings.
        const juce::ScopedValueSetter<bool> avSoundFile(avLoadingSoundFile, true);
        if (!loadAVStateInformation(source, sizeInBytes))
            return false;
    }
    // A preset file carries these too (it is a full state), but they are the
    // instrument's: the sound changes, the general volume does not.
    for (const auto& [parameter, kept] : { std::pair { avOutputParameter, output }, std::pair { avDelayTempoParameter, tempo } })
        if (parameter != nullptr && std::abs(parameter->getValue() - kept) > 1.0e-6f)
            parameter->setValueNotifyingHost(kept);
    if (layers.isValid()) {
        // The file's sound is its selected layer's, and its name stays the
        // sound's name (selecting a layer forgets it, as it does any other time).
        const auto name = avPresetName, id = avPresetId;
        const auto path = avPresetPath;
        applyAVLayersTree(layers);
        setAVPresetContext(name, id, path);
        captureAVPresetBaseline();
    }
    return true;
}

bool DexedAudioProcessor::loadAVStateInformation(const void* source, int sizeInBytes) {
    if (source == nullptr || sizeInBytes <= 0 || sizeInBytes > 32 * 1024 * 1024)
        return false;
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(source, sizeInBytes));
    if (xmlState == nullptr)
        return false;

    if (xmlState->hasTagName("dexedState")) {
        // Format 1.0 used a separate XML schema and binary program attribute.
        // Its bank file is read before the audio lock (a slow or sleeping disk
        // must not stall render); the bank is installed with its per-sound
        // settings, which are not applied: the restored values win.
        const auto legacySourcePath = xmlState->getStringAttribute("activeFileCartridge");
        const auto legacyBankFile = avSessionBankFile(legacySourcePath);
        Cartridge legacyBank;
        const bool legacyBankRead = AVDexedLibrary::readBank(legacyBankFile, legacyBank);
        const auto legacySoundTable = readAVSoundTable(legacyBankFile);

        juce::ScopedValueSetter<bool> disableSysexEcho(sendSysexChange, false);
        const juce::ScopedLock avStateLock(getCallbackLock());
        if (!setStateInformation10(this, xmlState.get()))
            return false;
        if (legacySourcePath.isNotEmpty())
            installAVActiveBank(legacyBankRead ? legacyBank : currentCart, legacyBankFile, legacySoundTable);
        // A 1.0 state does not say whether its voice still is the slot's own:
        // it was applied as a voice of its own (no known origin), so "Guardar"
        // goes to "Guardar como…" rather than over the bank voice.
        avSoundFromBankSlot.store(hasAVKnownPresetOrigin(), std::memory_order_relaxed);
        if (! avLoadingSoundFile)
            restoreAVLayersState({});   // a Dexed 1.0 state knows no layers: one layer
        return true;
    }

    if (!xmlState->hasTagName(parameters.rootVt.getType().toString())) {
        TRACE("No valid state found");
        return false;
    }

    auto storage = juce::ValueTree::fromXml(*xmlState);
    if (storage.hasProperty("nativePresetVersion")
        && static_cast<int>(storage.getProperty("nativePresetVersion")) != 1)
        return false;
    const auto version = storage.getProperty(IDs::version).toString();
    const bool supportsVersion = version == DexedApvts::MODEL_VERSION || version == "1.0.0";
    if (!supportsVersion) {
        TRACE("Unsupported state version %s (current is %s)", version.toRawUTF8(), DexedApvts::MODEL_VERSION);
        return false;
    }

    if (storage.hasProperty("nativePresetVersion") && static_cast<int>(storage.getProperty("nativePresetVersion")) != 1) {
        TRACE("Unsupported native preset format");
        return false;
    }
    auto parameterState = storage.getChildWithName(IDs::parameters);
    if (!parameterState.isValid() || parameterState.getType() != IDs::parameters) {
        TRACE("State has no valid APVTS parameter tree");
        return false;
    }
    parameterState = parameterState.createCopy();
    if (!avPrepareParameterState(parameters, parameterState)) {
        TRACE("State contains invalid or duplicate parameter values");
        return false;
    }

    auto schema = static_cast<int>(storage.getProperty("schemaVersion", 0));
    if (schema < 0 || schema > currentStateSchema) {
        TRACE("Unsupported state schema %d", schema);
        return false;
    }

    // Validate every snapshot payload before changing the live processor.
    auto session = storage.getChildWithName(sessionNode);
    juce::MemoryBlock decodedVoice;
    juce::MemoryBlock decodedCartridge;
    bool hasVoiceSnapshot = false;
    bool hasCartridgeSnapshot = false;
    int restoredProgram = currentProgram;
    juce::String restoredSourcePath;

    if (session.isValid()) {
        const auto encodedVoice = session.getProperty(voiceProperty).toString();
        if (!decodeFixedBlock(encodedVoice, Program::PROGRAM_SIZE + 1, decodedVoice)) {
            TRACE("State voice snapshot is missing or malformed");
            return false;
        }
        hasVoiceSnapshot = true;

        const auto encodedCartridge = session.getProperty(cartridgeProperty).toString();
        if (encodedCartridge.isNotEmpty()) {
            if (!decodeFixedBlock(encodedCartridge, SYSEX_SIZE, decodedCartridge)) {
                TRACE("State cartridge snapshot is malformed");
                return false;
            }
            hasCartridgeSnapshot = true;
        }

        restoredProgram = static_cast<int>(session.getProperty("currentProgram", -1));
        if (restoredProgram < 0 || restoredProgram >= 32) {
            TRACE("State program selection is outside the DX7 bank");
            return false;
        }
        restoredSourcePath = session.getProperty("sourcePath").toString();
    }

    auto storageProfile = storage.getChildWithName(IDs::profile);
    auto mappings = storageProfile.getChildWithName(IDs::midiCCMappings);
    if (storageProfile.isValid() && (!mappings.isValid() || mappings.getType() != IDs::midiCCMappings)) {
        TRACE("State MIDI mapping profile is malformed");
        return false;
    }
    std::set<int> avMappedCCs;
    for (const auto& mapping : mappings) {
        const int cc = mapping.getProperty(IDs::midiCC, -1);
        const auto id = mapping.getProperty(IDs::midiParameterName).toString();
        if (mapping.getType() != IDs::midiCCMapping || cc < 0 || cc > 127
            || parameters.getParameter(id) == nullptr || !avMappedCCs.insert(cc).second) {
            TRACE("State MIDI mapping entry is invalid");
            return false;
        }
    }

    auto tuningVt = storage.getChildWithName("tuning");
    juce::String sclData;
    juce::String kbmData;
    if (tuningVt.isValid()) {
        sclData = tuningVt.getProperty("scl").toString();
        kbmData = tuningVt.getProperty("kbm").toString();
        if (sclData.getNumBytesAsUTF8() > MAX_SCL_KBM_FILE_SIZE
            || kbmData.getNumBytesAsUTF8() > MAX_SCL_KBM_FILE_SIZE) {
            TRACE("State tuning data exceeds the supported size");
            return false;
        }
        if (!avValidateTuningData(sclData, kbmData)) {
            TRACE("State contains invalid tuning data");
            return false;
        }
    }

    Cartridge restoredCart;
    if (hasCartridgeSnapshot) {
        const auto result = restoredCart.load(static_cast<const uint8_t*>(decodedCartridge.getData()),
                                              static_cast<int>(decodedCartridge.getSize()));
        juce::MemoryBlock amem;
        if (amem.fromBase64Encoding(session.getProperty("cartridgeAmem").toString()) && amem.getSize() == 1120)
            restoredCart.setAmem(static_cast<const uint8_t*>(amem.getData()));
        if (result != 0 || !restoredCart.containsData()) {
            TRACE("State cartridge could not be restored");
            return false;
        }
    }

    // The session's bank file and its per-sound settings are read before the
    // audio lock. They are installed with the bank so later program changes
    // find them, but never applied here: the session's own values win.
    const auto restoredBankFile = session.isValid() ? avSessionBankFile(restoredSourcePath) : juce::File();
    const auto restoredSoundTable = readAVSoundTable(restoredBankFile);

    // State restoration must not echo parameter changes to an attached DX7.
    const juce::ScopedLock avStateLock(getCallbackLock());
    juce::ScopedValueSetter<bool> disableSysexEcho(sendSysexChange, false);
    // A sound file is one sound: in layer mode only the selected layer stops.
    if (avLoadingSoundFile) panicForSoundChange();
    else panic();
    // The parsed parameter tree is still parented by the temporary state XML
    // tree. Clone it before installing it into the live preference root so the
    // ValueTree is never reparented implicitly (which JUCE asserts against).
    parameters.replaceState(parameterState.createCopy());

    // replaceState() swaps the APVTS tree, so keep rootVt pointing at the live one.
    if (parameters.state.getParent() != parameters.rootVt) {
        auto oldState = parameters.rootVt.getChildWithName(IDs::parameters);
        if (oldState.isValid())
            parameters.rootVt.removeChild(oldState, nullptr);
        parameters.rootVt.addChild(parameters.state, 0, nullptr);
    }

    // The stored list wins, even an empty one the user left (a list saved
    // before CC1 → Cutoff existed gets it). A session's arrow CCs come with
    // it; a sound file keeps the live ones.
    if (mappings.isValid())
        midiCCMapper->restoreAVMappings(mappings, avLoadingSoundFile);

    if (hasVoiceSnapshot) {
        Program restoredVoice(static_cast<const uint8_t*>(decodedVoice.getData()));
        activeProgram = restoredVoice;
        // Whatever the file holds, the voice follows the bank rule first (safe
        // values for the engine's tables), as a bank or SysEx voice does; the
        // reconciliation below then compares the parameters with that voice.
        Cartridge::applyBankRules(activeProgram);

        // APVTS is authoritative for automatable values. Reconcile the raw DX
        // buffer with the restored parameter tree while retaining voice data
        // such as its DX7 name that is not represented by a host parameter.
        for (const auto& meta : dxParameters) {
            if (auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(parameters.getParameter(meta.name))) {
                auto value = static_cast<int>(std::lround(parameter->convertFrom0to1(parameter->getValue())));
                // A bank value beyond the parameter's range (an output level of
                // 102, as real DX7 banks hold) shows as the range's end. While
                // the parameter still sits at that end, the voice keeps its own
                // value, so a reopened project sounds as it did when saved.
                const auto& range = parameter->getNormalisableRange();
                const int stored = activeProgram[meta.pos];
                const int top = static_cast<int>(std::lround(range.end)) + meta.displayOffset;
                const int bottom = static_cast<int>(std::lround(range.start)) + meta.displayOffset;
                if ((value + meta.displayOffset == top && stored > top) || (value + meta.displayOffset == bottom && stored < bottom))
                    continue;
                activeProgram[meta.pos] = static_cast<uint8_t>(juce::jlimit(0, 127, value + meta.displayOffset));
            }
        }
        // The parameters only ever put in-range values back: still within the rule.
        for (int op = 0; op < 6; ++op) {
            auto* parameter = parameters.getParameter(IDs::on.op(op).name);
            if (parameter != nullptr) {
                const auto mask = static_cast<uint8_t>(1U << (5 - op));
                if (parameter->getValue() > 0.5f)
                    activeProgram[155] |= mask;
                else
                    activeProgram[155] &= static_cast<uint8_t>(~mask);
            }
        }
    }

    if (hasCartridgeSnapshot) {
        installAVActiveBank(restoredCart, restoredBankFile, restoredSoundTable);
        setCurrentProgramSelection(restoredProgram);
    } else if (session.isValid()) {
        setCurrentProgramSelection(restoredProgram);
        installAVActiveBank(currentCart, restoredBankFile, restoredSoundTable);
    }

    refreshVoice.set(true);

    avPresetName = session.getProperty("avPresetName").toString();
    avPresetId = session.getProperty("avPresetId").toString();
    const auto presetPath = session.getProperty("avPresetPath").toString();
    // A session saved before the sound files were renamed (.dexedpreset) finds
    // the new name, but only when that file holds the same sound: a name clash
    // at the rename left the old sound as "<name> 2" (the browser finds it by id).
    // A path from another OS ("/Users/..." on Windows) is no file here.
    avPresetPath = presetPath.isEmpty() || ! juce::File::isAbsolutePath(presetPath) ? juce::File() : juce::File(presetPath);
    if (const auto renamed = AVDexedLibrary::remapPresetPath(avPresetPath); renamed != avPresetPath) {
        juce::String renamedId;
        juce::MemoryBlock renamedData;
        if (avPresetId.isNotEmpty() && renamed.getSize() <= 32 * 1024 * 1024 && renamed.loadFileAsData(renamedData))
            if (auto xml = getXmlFromBinary(renamedData.getData(), static_cast<int>(renamedData.getSize())))
                renamedId = xml->getStringAttribute("libraryPresetId").trim();
        if (avPresetId.isEmpty() || renamedId == avPresetId.trim())
            avPresetPath = renamed;
    }
    avPresetContextCurrent.store(avPresetName.isNotEmpty() || avPresetId.isNotEmpty() || presetPath.isNotEmpty(),
                                 std::memory_order_relaxed);
    avKnownPresetOrigin.store(session.isValid() && static_cast<bool>(session.getProperty("avKnownPresetOrigin", true)),
                              std::memory_order_relaxed);
    // Sessions saved before this property existed (0.1.0, early 0.2.0 builds)
    // do not store it: the restored origin tells (Init and a single SysEx
    // voice cleared both). Never "a slot" by default, or "Guardar" would write
    // an Init sound over the bank voice.
    avSoundFromBankSlot.store(session.isValid()
                                  && static_cast<bool>(session.getProperty("avSoundFromBankSlot", hasAVKnownPresetOrigin())),
                              std::memory_order_relaxed);
    if (!session.isValid()) {
        activeProgram.setName({});
        installAVActiveBank(currentCart, juce::File(), AVSoundTable {});
    }

    if (tuningVt.isValid()) {
        retuneToStandard();
        if (sclData.isNotEmpty())
            applySCLTuning(sclData.toStdString());
        if (kbmData.isNotEmpty())
            applyKBMMapping(kbmData.toStdString());
    }
    const auto baseline = session.getProperty("avPresetBaseline").toString();
    avPresetBaseline.store(static_cast<uint64_t>(baseline.getHexValue64()), std::memory_order_relaxed);
    // A baseline hashed by another version (0.1.0 also hashed the general
    // volume) cannot be compared: the restored sound counts as unmodified.
    if (baseline.isEmpty() || avPresetBaseline.load(std::memory_order_relaxed) == 0
        || static_cast<int>(session.getProperty("avPresetBaselineFormat", 1)) != avPresetBaselineFormat)
        captureAVPresetBaseline();
    // A restored sound: no CC has moved anything of it yet.
    if (midiCCMapper != nullptr) midiCCMapper->forgetAVCCWrites();
    // The layers belong to the session; a sound file loads into the selected layer.
    if (avLoadingSoundFile) {
        avLayers[size_t(avSelectedLayer.load(std::memory_order_acquire))].setProgram(activeProgram);
        setAVLayerVoiceExtras(avSelectedLayer.load(std::memory_order_acquire), nullptr);
    }
    else
        restoreAVLayersState(storage.getChildWithName("avLayers"));
    programChanged.set(true);
    return true;
}


juce::MemoryBlock DexedAudioProcessor::createAVNativePresetState(const juce::String& name, const juce::String& id, bool withLayers) {
    const juce::ScopedLock avStateLock(getCallbackLock());
    juce::MemoryBlock bytes;
    getStateInformation(bytes);
    auto xml = getXmlFromBinary(bytes.getData(), static_cast<int>(bytes.getSize()));
    if (xml == nullptr) return {};
    auto tree = juce::ValueTree::fromXml(*xml);
    tree.setProperty("nativePresetVersion", 1, nullptr);
    tree.setProperty("libraryPresetId", id, nullptr);
    // A sound file keeps the sound's own values, not where the mod wheel left
    // a parameter it drives (CC1 → Cutoff by default).
    if (midiCCMapper != nullptr)
        if (auto values = tree.getChildWithName(parameters.state.getType()); values.isValid())
            for (auto* parameter : getParameters())
                if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter)) {
                    const float soundValue = midiCCMapper->getAVSoundValue(ranged);
                    if (std::abs(soundValue - ranged->getValue()) > 1.0e-6f)
                        for (auto child : values)
                            if (child.getProperty("id").toString() == ranged->paramID)
                                child.setProperty("value", ranged->convertFrom0to1(soundValue), nullptr);
                }
    // A sound file does not carry the arrow CCs (loading one keeps the live ones).
    auto mappings = tree.getChildWithName(IDs::profile).getChildWithName(IDs::midiCCMappings);
    mappings.removeProperty(IDs::avPresetPreviousCC, nullptr);
    mappings.removeProperty(IDs::avPresetNextCC, nullptr);
    if (auto layers = tree.getChildWithName("avLayers"); ! withLayers)
        tree.removeChild(layers, nullptr);   // a sound, not a set of layers
    else {
        layers.setProperty("mode", true, nullptr);   // played as layers, whatever the mode now
        layers.removeProperty("setPath", nullptr);   // the file itself
        layers.removeProperty("setId", nullptr);
    }
    auto session = tree.getChildWithName(sessionNode);
    session.setProperty("avPresetName", name, nullptr);
    session.setProperty("avPresetId", id, nullptr);
    session.setProperty("avKnownPresetOrigin", true, nullptr);
    session.setProperty("avPresetPath", juce::String(), nullptr);
    session.setProperty("avPresetBaseline", juce::String::toHexString(static_cast<juce::int64>(getAVMusicalFingerprint(&name))), nullptr);
    session.setProperty("avPresetBaselineFormat", avPresetBaselineFormat, nullptr);
    if (auto nativeXml = tree.createXml()) copyXmlToBinary(*nativeXml, bytes);
    return bytes;
}
