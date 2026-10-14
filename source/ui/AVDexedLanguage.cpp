// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedLanguage.h"
#include "../JuceLibraryCode/BinaryData.h"

using namespace juce;

AVDexedLanguageService& AVDexedLanguageService::getInstance() {
    static AVDexedLanguageService service;
    return service;
}

AVDexedLanguageService::AVDexedLanguageService()
    : english(parseCatalog(String::fromUTF8(BinaryData::en_txt, BinaryData::en_txtSize))),
      spanish(parseCatalog(String::fromUTF8(BinaryData::es_txt, BinaryData::es_txtSize))) {
    // Validate the embedded pair without changing JUCE's process-wide mappings.
    for (const auto& key : english.getAllKeys()) {
        jassert(spanish.containsKey(key));
        auto placeholders = [](const String& value) {
            StringArray names;
            for (int i = 0; i < value.length(); ++i) {
                if (value[i] != '$')
                    continue;
                int end = i + 1;
                while (end < value.length()
                       && (CharacterFunctions::isLetterOrDigit(value[end]) || value[end] == '_'))
                    ++end;
                if (end > i + 1)
                    names.addIfNotAlreadyThere(value.substring(i, end));
                i = end - 1;
            }
            names.sort(true);
            return names;
        };
        if (placeholders(english[key]) != placeholders(spanish[key])) {
            jassertfalse;
            spanish.set(key, english[key]);
        }
    }
}

StringPairArray AVDexedLanguageService::parseCatalog(const String& contents) {
    StringPairArray parsed;
    for (const auto& line : StringArray::fromLines(contents)) {
        const auto separator = line.indexOfChar('=');
        if (separator <= 0)
            continue;
        parsed.set(line.substring(0, separator).trim(), line.substring(separator + 1).replace("\\n", "\n"));
    }
    return parsed;
}

void AVDexedLanguageService::configure(const File& applicationDirectory) {
    const auto nextPreferenceFile = applicationDirectory.getChildFile("Dexed.xml");
    if (preferenceFile == nextPreferenceFile)
        return;

    preferenceFile = nextPreferenceFile;
    PropertiesFile::Options options;
    PropertiesFile preferences(preferenceFile, options);
    const auto stored = preferences.getValue("avDexedLanguage", "es");
    const String loaded = stored == "en" ? "en" : "es";
    if (loaded == language)
        return;
    // Components built before the preference was read (e.g. the bank manager)
    // must hear about it, or they keep the default language's captions.
    language = loaded;
    sendChangeMessage();
}

void AVDexedLanguageService::setLanguage(const String& languageCode) {
    const auto nextLanguage = languageCode == "en" ? String("en") : String("es");
    if (language == nextLanguage)
        return;

    language = nextLanguage;
    if (preferenceFile != File()) {
        PropertiesFile::Options options;
        PropertiesFile preferences(preferenceFile, options);
        preferences.setValue("avDexedLanguage", language);
        preferences.save();
    }
    sendChangeMessage();
}

String AVDexedLanguageService::text(const String& key) const {
    const auto& active = isSpanish() ? spanish : english;
    return active.getValue(key, english.getValue(key, key));
}

String AVDexedLanguageService::format(const String& key,
                                    std::initializer_list<std::pair<String, String>> values) const {
    const auto translated = text(key);
    String result;
    for (int i = 0; i < translated.length();) {
        if (translated[i] != '$') {
            result += translated.substring(i, i + 1);
            ++i;
            continue;
        }
        int end = i + 1;
        while (end < translated.length()
               && (CharacterFunctions::isLetterOrDigit(translated[end]) || translated[end] == '_'))
            ++end;
        const auto name = translated.substring(i + 1, end);
        bool inserted = false;
        for (const auto& value : values)
            if (value.first == name) {
                result += value.second;
                inserted = true;
                break;
            }
        if (!inserted)
            result += translated.substring(i, end);
        i = jmax(i + 1, end);
    }
    return result;
}

String AVDexedLanguageService::parameterName(const String& parameterId, const String& fallback) const {
    String id = parameterId, prefix, suffix;
    if (id.startsWith("op") && id.length() > 3 && id[2] >= '1' && id[2] <= '6') {
        prefix = text("operator") + " " + id.substring(2, 3) + String::fromUTF8(" · ");
        id = id.substring(3);
    }
    if (id.startsWith("egRate") || id.startsWith("egLevel") || id.startsWith("pitchEgRate") || id.startsWith("pitchEgLevel")) {
        suffix = " " + id.getLastCharacters(1);
        id = id.dropLastCharacters(1);
        if (id.startsWith("pitch")) prefix = text("PITCH_ENVELOPE") + String::fromUTF8(" · ");
    }
    const std::pair<const char*, const char*> names[] {
        {"detune", "DETUNE"}, {"frequencyCoarse", "COARSE"}, {"frequencyFine", "FINE"},
        {"mode", "FREQUENCY_MODE"}, {"on", "operatorToggleTitle"}, {"ampModeSens", "AMP_MOD"},
        {"keyVelocity", "KEY_VEL"}, {"outputLevel", "LEVEL"}, {"egRate", "RATE"}, {"egLevel", "LEVEL"},
        {"pitchEgRate", "RATE"}, {"pitchEgLevel", "LEVEL"}, {"breakpoint", "BREAKPOINT"},
        {"lScaleDepth", "LEFT_DEPTH"}, {"rScaleDepth", "RIGHT_DEPTH"}, {"lKeyScale", "LEFT_CURVE"},
        {"rKeyScale", "RIGHT_CURVE"}, {"rateScaling", "RATE_SCALE"}, {"algorithm", "ALGORITHM"},
        {"feedback", "FEEDBACK"}, {"oscKeySync", "OSC_SYNC"}, {"lfoRate", "SPEED"},
        {"lfoDelay", "DELAY"}, {"lfoPmDepth", "PMD"}, {"lfoAmpDepth", "AMD"}, {"lfoKeySync", "KEY_SYNC"},
        {"lfoWaveform", "LFO_WAVE"}, {"pitchModSens", "PM_SENS"}, {"transpose", "TRANSPOSE"},
        {"output", "OUTPUT"}, {"monoMode", "MONO"}, {"masterTuneAdj", "MASTER_TUNE"},
        {"cutoff", "CUTOFF"}, {"resonance", "RESONANCE"}, {"pitchRangeUp", "pitchBendUp"},
        {"pitchRangeDown", "pitchBendDown"}, {"pitchStep", "pitchBendStep"},
        {"transposeTuningScale", "transposeScale"}, {"glissando", "glissando"}, {"portamentoTm", "portamentoRate"},
        {"engineType", "engineResolution"}, {"masterVolume", "volume"}, {"pan", "pan"},
        {"avDelayTempo", "localTempo"}
    };
    for (const auto& name : names)
        if (id == name.first) return prefix + text(name.second) + suffix;
    const std::pair<const char*, const char*> controllerNames[] { {"modWheel", "wheel"}, {"modFoot", "foot"},
        {"modBreath", "breath"}, {"modAftertouch", "aftertouch"} };
    for (const auto& controller : controllerNames)
        if (id.startsWith(controller.first)) {
            const auto tail = id.substring(String(controller.first).length());
            const auto key = tail == "Pitch" ? "modPitch" : tail == "Amp" ? "modAmplitude"
                           : tail == "EgBias" ? "modEgBias" : "modRange";
            return text(controller.second) + String::fromUTF8(" · ") + text(key);
        }
    const std::pair<const char*, const char*> effects[] {{"reverb", "reverb"}, {"delay", "delay"}, {"chorus", "chorus"}};
    for (const auto& effect : effects)
        if (id.startsWith(effect.first)) {
            const auto tail = id.substring(String(effect.first).length());
            const auto key = tail == "Amount" ? "amount" : tail == "Size" ? "sizeDecay"
                : tail == "PreDelay" ? "preDelay" : tail == "Damping" ? "damping" : tail == "Sync" ? "tempoSync"
                : tail == "DivisionLeft" ? "leftDivision" : tail == "DivisionRight" ? "rightDivision"
                : tail == "TimeLeft" ? "leftTime" : tail == "TimeRight" ? "rightTime"
                : tail == "Feedback" ? "feedback" : tail == "Mode" ? "mode" : tail == "Rate" ? "rate"
                : tail == "Depth" ? "depth" : tail == "Spread" ? "stereoSpread" : "";
            if (String(key).isNotEmpty()) return text(effect.second) + String::fromUTF8(" · ") + text(key);
        }
    if (id == "avReverbBypass") return text("reverb") + String::fromUTF8(" · ") + text("bypass");
    if (id == "avDelayBypass") return text("delay") + String::fromUTF8(" · ") + text("bypass");
    if (id == "avChorusBypass") return text("chorus") + String::fromUTF8(" · ") + text("bypass");
    return fallback;
}
