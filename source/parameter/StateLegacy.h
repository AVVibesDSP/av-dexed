// Used for parsing old Dexed 1.0 legacy format

#pragma once

#include "../PluginProcessor.h"
#include "Model.h"

static void setApvtsParam(DexedApvts& apvts, const juce::String& paramId, float value) {
    auto* param = dynamic_cast<juce::RangedAudioParameter*>(apvts.getParameter(paramId));
    if (param)
        param->setValueNotifyingHost(param->convertTo0to1(value));
}

static void setApvtsParamBool(DexedApvts& apvts, const juce::String& paramId, bool value) {
    auto* param = apvts.getParameter(paramId);
    if (param)
        param->setValueNotifyingHost(value ? 1.0f : 0.0f);
}

/** The bank file a saved session points to. Sessions up to 0.1.0 point into
    Cartridges/, which 0.2.0 absorbed into the library: map them to where the
    migration put the file. Relative or empty paths mean no file. */
static juce::File avSessionBankFile(const juce::String& path) {
    if (path.isEmpty() || ! juce::File::isAbsolutePath(path))
        return {};
    const juce::File file(path);
    const auto appDirectory = DexedAudioProcessor::dexedAppDir;
    return appDirectory == juce::File() ? file : AVDexedLibrary::remapLegacyPath(file, appDirectory);
}

bool setStateInformation10(DexedAudioProcessor* proc, XmlElement* root) {
    auto& apvts = proc->parameters;

    // Validate the complete legacy payload before changing a live parameter.
    // A short blob previously made Program read 155 bytes past its allocation.
    const auto* dexedBlob = root->getChildByName("dexedBlob");
    NamedValueSet blobSet;
    if (dexedBlob == nullptr)
        return false;
    blobSet.setFromXmlAttributes(*dexedBlob);
    const auto program = blobSet["program"];
    const auto* programData = program.getBinaryData();
    if (programData == nullptr || programData->getSize() < Program::PROGRAM_SIZE
        || programData->getSize() > Program::PROGRAM_SIZE + 1)
        return false;

    juce::String sclData, kbmData;
    const auto* tuningParent = root->getChildByName("dexedTuning");
    if (tuningParent != nullptr) {
        if (const auto* scl = tuningParent->getChildByName("scl")) sclData = scl->getAllSubText();
        if (const auto* kbm = tuningParent->getChildByName("kbm")) kbmData = kbm->getAllSubText();
        if (sclData.getNumBytesAsUTF8() > MAX_SCL_KBM_FILE_SIZE
            || kbmData.getNumBytesAsUTF8() > MAX_SCL_KBM_FILE_SIZE)
            return false;
        try {
            if (sclData.isNotEmpty() && kbmData.isNotEmpty()) {
                Tunings::Tuning tuning(Tunings::parseSCLData(sclData.toStdString()), Tunings::parseKBMData(kbmData.toStdString()));
            } else if (sclData.isNotEmpty()) {
                Tunings::Tuning tuning(Tunings::parseSCLData(sclData.toStdString()));
            } else if (kbmData.isNotEmpty()) {
                Tunings::Tuning tuning(Tunings::parseKBMData(kbmData.toStdString()));
            }
        } catch (const std::exception&) { return false; }
    }
    for (const auto* name : { "cutoff", "reso", "gain", "engineType", "monoMode", "transpose12AsScale", "glissando", "portamento", "currentProgram" }) {
        if (root->hasAttribute(name) && !std::isfinite(root->getDoubleAttribute(name)))
            return false;
    }
    const int legacyProgram = root->getIntAttribute("currentProgram", proc->getCurrentProgram());
    if (legacyProgram < 0 || legacyProgram >= proc->getNumPrograms())
        return false;

    proc->panic();
    bool newParameters = false;
    for (auto* parameter : proc->getParameters()) {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter);
        if (ranged == nullptr) continue;
        newParameters |= ranged->paramID == IDs::masterVolume.name;
        if (newParameters)
            ranged->setValueNotifyingHost(ranged->getDefaultValue());
    }

    setApvtsParam(apvts, IDs::cutoff.name, (float)root->getDoubleAttribute("cutoff", 1.0));
    setApvtsParam(apvts, IDs::resonance.name, (float)root->getDoubleAttribute("reso", 0.0));
    setApvtsParam(apvts, IDs::output.name, (float)root->getDoubleAttribute("gain", 1.0));
    setApvtsParam(apvts, IDs::engineType.name, (float)root->getIntAttribute("engineType", 0));
    setApvtsParamBool(apvts, IDs::monoMode.name, root->getIntAttribute("monoMode", 0) != 0);
    setApvtsParamBool(apvts, IDs::transposeTuningScale.name, root->getIntAttribute("transpose12AsScale", 1) != 0);
    setApvtsParamBool(apvts, IDs::glissando.name, root->getIntAttribute("glissando", 0) != 0);

    // Old format: portamento 0-127, new APVTS: portamentoTm 0-99
    int oldPortamento = root->getIntAttribute("portamento", 0);
    setApvtsParam(apvts, IDs::portamentoTm.name, oldPortamento * 99.0f / 127.0f);

    // Load program data from dexedBlob
    Program legacyVoice(static_cast<const uint8_t*>(programData->getData()));
    Cartridge::applyBankRules(legacyVoice);   // the same rule as a bank voice
    proc->applyProgram(legacyVoice);

    // Restore the visible context without loading a slot over the voice that
    // was embedded in the legacy state.
    if (legacyProgram >= 0 && legacyProgram < proc->getNumPrograms())
        proc->setCurrentProgramSelection(legacyProgram);

    // The bank of "activeFileCartridge" is read by the caller before the audio
    // lock and installed after this function (loadAVStateInformation).

    // Load tuning from dexedTuning child
    if (tuningParent != nullptr) {
        proc->retuneToStandard();
        if (sclData.isNotEmpty()) proc->applySCLTuning(sclData.toStdString());
        if (kbmData.isNotEmpty()) proc->applyKBMMapping(kbmData.toStdString());
    }
    proc->captureAVPresetBaseline();
    return true;
}
