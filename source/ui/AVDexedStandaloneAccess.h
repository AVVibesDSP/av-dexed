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
}
