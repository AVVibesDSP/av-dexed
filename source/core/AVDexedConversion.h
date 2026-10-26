// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "AVDexedLibrary.h"
#include "AVDexedPerformance.h"
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
    juce::String id;   // the sound file's libraryPresetId
};
/** Makes the sound file of `bank`'s voice `slot`, named `name`, at `order`
    in its folder. All the voices of one bank come in a row. */
using SoundMaker = std::function<MadeSound(const AVDexedLibrary::SourceBank& bank, int slot,
                                           const juce::String& name, int order)>;

/** Makes the sound file of a DX7II or TX802 performance: a layers preset of
    its eight layers (`layers`, as the Capas page plays them), named `name`,
    from performance `number` of `sourceName`, at `order` in its folder. */
using LayersMaker = std::function<MadeSound(const std::array<AVDexedPerformance::ResolvedLayer, AVDexedPerformance::layerCount>& layers,
                                            const juce::String& name, const juce::String& sourceName, int number, int order)>;

/** Where the voices go. Single voices (files of one voice each) are never a
    folder of their own: they go straight into their folder, whatever the layout. */
enum class Layout {
    folderPerBank,   // a folder per bank inside the destination ("Rhodes/…")
    flat,            // every voice directly in the destination
    automatic,       // flat for one bank in all, a folder per bank for more
    besideSources    // each file's voices in the folder the file is in (the library's own banks; no destination)
};
// The performances of a file are a folder like a bank: "<file>", or
// "<file> Performances" when the file holds banks too. Their voices come from
// the file's own banks or from the files next to it (AVDexedPerformance::VoicePool).

struct Request {
    juce::Array<juce::File> sources;   // .syx files of the DX7 family, in the order to convert
    juce::File root;                   // the library root (the backups go into it)
    juce::File destination;            // a folder of the library (created when missing)
    Layout layout = Layout::folderPerBank;
    juce::File sourceBase;             // a dropped folder: the backup keeps the paths below it
    juce::String backupLabel;          // names the backup folder ("2026-10-25 2134 <label>")
    bool backup = true;
    /** The originals move into the backup instead of being copied there: the
        library's own banks, which must not stay in it as banks. The backup
        then keeps a record of the conversion (undoConversion), and the library
        a record of where each bank voice went (convertedVoice). */
    bool moveSources = false;
};

/** Where a bank voice went: its key in the browser ("bank:<fingerprint>:<slot>",
    as its favourite) and the sound file that plays it now, with that file's
    key ("user:<id>"). A repeated voice goes to the sound already there. */
struct Origin {
    juce::String bankKey;
    juce::File file;
    juce::String key;
};

struct Result {
    bool ok = false;
    bool cancelled = false;            // `progress` asked to stop
    int banks = 0;                     // banks read
    int performances = 0;              // performances read (with `makeLayers`)
    int sounds = 0;                    // sound files written
    int repeated = 0;                  // voices skipped: the same sound is in the folder already
    int empty = 0;                     // voices skipped: the Init voice
    juce::StringArray unreadable;      // file names with no voice of the DX7 family
    juce::Array<juce::File> folders;   // the folders that received sounds
    juce::Array<juce::File> files;     // the sound files written
    juce::File backup;                 // the backup folder, when one was made
    std::vector<Origin> origins;       // every bank voice converted or repeated
};

/** Converts `request.sources`. Each sound file is written into a hidden
    staging folder first; only once every one of them is written do they move
    into the library, under the library lock. When anything fails, nothing
    reaches the library and no backup is left. `progress` (0–1) returns false
    to cancel. */
Result convert(const Request& request, const SoundMaker& make, const LayersMaker& makeLayers = {},
               const std::function<bool(float)>& progress = {});

/** The key a converted sound file keeps (session "avSoundKey"); 0 for any
    other sound file. */
juce::uint64 readSoundKey(const juce::File& soundFile);

/** The sound file a bank voice of the library became ("bank:<fingerprint>:<slot>"),
    while it is there; File() otherwise. `key`, when given, receives its key
    ("user:<id>"). A DAW session that played the bank voice finds it this way. */
juce::File convertedVoice(const juce::File& root, const juce::String& bankKey, juce::String* key = nullptr);

/** A conversion with moveSources undone from its backup: the original files go
    back where they were (when nothing else is there now), and the sound files
    it made go, those that nobody changed since (a sound saved over is kept). */
struct UndoResult {
    bool ok = false;
    int restored = 0, removed = 0, kept = 0;
    std::vector<Origin> origins;       // the bank voices that are banks again
};
UndoResult undoConversion(const juce::File& root, const juce::File& backup);

/** The newest backup of a conversion that can be undone (one made with
    moveSources, its record still there); File() when there is none. */
juce::File lastUndoableConversion(const juce::File& root);
}
