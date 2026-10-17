// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <functional>

// Populated only by our Standalone application. Plugin instances always let
// the host own the audio device; no second device manager is created by a UI.
namespace AVDexedStandaloneAccess {
inline juce::AudioDeviceManager* AVDeviceManager = nullptr;
inline std::function<void()> AVOpenAudioSettings;
/** Set by the Standalone's editor: opens AV Dexed's own Settings, so the app
    menu never shows JUCE's stock audio dialog (or its Bluetooth MIDI pairing). */
inline std::function<void()> AVShowProductSettings;
/** Set by the Standalone's editor: files the system asks the app to open (a
    double-click in the Finder, a drop on the Dock icon, "Open with", the
    command line on Windows). Banks, sounds and folders of them are imported
    like a drop on the window. */
inline std::function<void(const juce::StringArray&)> AVOpenFiles;

/** The paths of a command line as JUCE hands it over: space separated, quoted
    when they hold spaces, a quote inside a name escaped as \" (macOS:
    quotedIfContainsSpaces). The absolute ones that exist, in order, once each;
    flags and the "-psn_…" token macOS adds are left out. */
inline juce::StringArray AVSystemOpenPaths(const juce::String& commandLine) {
    juce::StringArray paths;
    juce::String token;
    bool inQuotes = false;
    const auto endToken = [&paths, &token] {
        const auto path = token.trim();
        token.clear();
        if (path.isNotEmpty() && juce::File::isAbsolutePath(path) && juce::File(path).exists())
            paths.addIfNotAlreadyThere(path);
    };
    for (auto text = commandLine.getCharPointer(); ! text.isEmpty();) {
        const auto c = text.getAndAdvance();
        if (c == '\\' && *text == '"') { token += juce::String::charToString(text.getAndAdvance()); }
        else if (c == '"') inQuotes = ! inQuotes;
        else if (! inQuotes && juce::CharacterFunctions::isWhitespace(c)) endToken();
        else token += juce::String::charToString(c);
    }
    endToken();
    return paths;
}
}
