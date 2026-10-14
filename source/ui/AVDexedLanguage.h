// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <initializer_list>
#include <utility>

/** Product-scoped live language preference. It deliberately does not install
    JUCE's process-wide LocalisedStrings mappings, so the host is unaffected. */
class AVDexedLanguageService final : public juce::ChangeBroadcaster {
public:
    static AVDexedLanguageService& getInstance();

    void configure(const juce::File& applicationDirectory);
    void setLanguage(const juce::String& languageCode);
    juce::String getLanguage() const { return language; }
    bool isSpanish() const { return language == "es"; }
    juce::String text(const juce::String& key) const;
    /** Translate the complete template first, then insert each value once.
        Values (preset names/paths) are data and are never parsed as templates. */
    juce::String format(const juce::String& key,
                       std::initializer_list<std::pair<juce::String, juce::String>> values) const;
    juce::String parameterName(const juce::String& parameterId, const juce::String& fallback) const;

private:
    AVDexedLanguageService();
    static juce::StringPairArray parseCatalog(const juce::String& contents);

    juce::String language { "es" };
    juce::File preferenceFile;
    juce::StringPairArray english;
    juce::StringPairArray spanish;
};
