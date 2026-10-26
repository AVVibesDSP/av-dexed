// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "AVDexedLibrary.h"
#include <functional>

/** Banks of the DX7 family turned into sound files: one .avdexpreset per
    voice, so the library holds sounds, not banks. The original files are kept
    in UserSYXsBackups, where nothing reads them as sounds.

    The sound file of each voice is made by the caller (the processor loads
    the voice the way the browser does and saves it the way "Guardar como"
    does); everything else (names, folders, repeated voices, order, backup,
    writing) is here, without the processor or the UI. */
namespace AVDexedConversion {

/** The names a bank's first `count` voices get as sounds: trailing spaces
    gone, and the slot number some banks put in front of every name ("26 BAS
    Ana" in slot 27) taken off, when the bank numbers most of its voices that
    way (from 0 or from 1). A name with nothing left is `fallback` + its slot. */
juce::StringArray voiceNames(const Cartridge& bank, int count, const juce::String& fallback);

/** A voice as a sound file, and the identity of its sound (the voice without
    its name, with its settings): two voices with the same key are the same sound. */
struct MadeSound {
    juce::MemoryBlock state;
    juce::uint64 key = 0;
};
/** Makes the sound file of `bank`'s voice `slot`, named `name`, at `order`
    in its folder. All the voices of one bank come in a row. */
using SoundMaker = std::function<MadeSound(const AVDexedLibrary::SourceBank& bank, int slot,
                                           const juce::String& name, int order)>;

enum class Layout {
    folderPerBank,   // a folder per bank inside the destination ("Rhodes/…")
    flat             // every voice directly in the destination
};

struct Request {
    juce::Array<juce::File> sources;   // .syx files of the DX7 family, in the order to convert
    juce::File root;                   // the library root (the backups go into it)
    juce::File destination;            // a folder of the library (created when missing)
    Layout layout = Layout::folderPerBank;
    juce::File sourceBase;             // a dropped folder: the backup keeps the paths below it
    juce::String backupLabel;          // names the backup folder ("2026-10-25 2134 <label>")
    bool backup = true;
};

struct Result {
    bool ok = false;
    int banks = 0;                     // banks read
    int sounds = 0;                    // sound files written
    int repeated = 0;                  // voices skipped: the same sound is in the folder already
    int empty = 0;                     // voices skipped: the Init voice
    juce::StringArray unreadable;      // file names with no voice of the DX7 family
    juce::Array<juce::File> folders;   // the folders that received sounds
    juce::Array<juce::File> files;     // the sound files written
    juce::File backup;                 // the backup folder, when one was made
};

/** Converts `request.sources`. Each sound file is written into a hidden
    staging folder first; only once every one of them is written do they move
    into the library, under the library lock. When anything fails, nothing
    reaches the library and no backup is left. `progress` (0–1) returns false
    to cancel. */
Result convert(const Request& request, const SoundMaker& make, const std::function<bool(float)>& progress = {});

/** The key a converted sound file keeps (session "avSoundKey"); 0 for any
    other sound file. */
juce::uint64 readSoundKey(const juce::File& soundFile);
}
