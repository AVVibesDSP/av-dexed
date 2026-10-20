// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <vector>

/** What a file (or a MIDI stream) of the DX7 family holds, found wherever it
    is: messages after a librarian's header, several banks in one file, a
    cartridge saved without its SysEx header, the last F7 missing, the
    DX7II/TX802 universal dumps that repeat several entries before one F7.

    Only reads: nothing here changes how a voice sounds. The formats are those
    of Yamaha's "DX7IIFD/D MIDI Technical Data" booklet and "TX802 MIDI System
    Exclusive Messages", checked against real files (see the research notes). */
namespace AVDexedSysex {

constexpr int bankBytes = 4096;        // VMEM: 32 packed voices of 128 bytes
constexpr int voiceBytes = 155;        // VCED: one unpacked voice
constexpr int acedBytes = 49;          // DX7II ACED: one voice's additional parameters
constexpr int amemBytes = 1120;        // DX7II AMEM: 32 packed ACED of 35 bytes
constexpr int amemVoiceBytes = 35;
constexpr int tx7PerformanceBytes = 4096;  // TX7/TF1/DX1/DX5 PMEM: 64 performances of 64 bytes

/** A universal bulk entry (`F0 43 0n 7E … "LM  " + 6 characters …`): the
    6-character name after "LM  " and its data bytes as sent (ASCII-hex
    nibbles for the TX802 performances and the fractional scaling). */
struct UniversalEntry {
    juce::String id;                   // e.g. "8973PM", "8952PM", "MCRYM\x01" is kept as "MCRYM1"
    std::vector<uint8_t> data;
    bool checksumOk = false;
};

struct Bank {
    std::array<uint8_t, bankBytes> voices {};
    bool checksumOk = true;
    /** The DX7II/TX802 additional data sent with this bank (the AMEM right
        before it in a bank dump), when there is one. */
    bool hasAmem = false;
    std::array<uint8_t, amemBytes> amem {};
    bool fromRawCartridge = false;     // 4096 bytes without a SysEx header
};

struct Contents {
    std::vector<Bank> banks;                                    // in file order
    std::vector<std::array<uint8_t, voiceBytes>> voices;        // single voices (VCED)
    std::vector<std::array<uint8_t, acedBytes>> acedForVoices;  // ACED sent right before each voice, or none
    std::vector<bool> voiceHasAced;
    std::vector<std::array<uint8_t, tx7PerformanceBytes>> tx7Performances;
    std::vector<UniversalEntry> universal;
    std::vector<std::array<uint8_t, amemBytes>> unpairedAmem;   // an AMEM with no bank after it

    bool isEmpty() const noexcept {
        return banks.empty() && voices.empty() && tx7Performances.empty() && universal.empty() && unpairedAmem.empty();
    }
    /** The entries of one universal kind ("8973PM"…), in file order. The
        pointers are into this Contents: keep it alive while using them. */
    std::vector<const UniversalEntry*> universalNamed(const juce::String& id) const;
};

/** Everything of the DX7 family in `data`. */
Contents scan(const uint8_t* data, size_t size);
Contents scan(const juce::MemoryBlock& data);

/** True for 4096 bytes that read as a DX7 cartridge saved without its SysEx
    header: every byte a MIDI data byte and the 32 names readable. */
bool looksLikeRawCartridge(const uint8_t* data, size_t size);

/** The 7-bit two's complement checksum of DX7 bulk data. */
uint8_t checksum(const uint8_t* data, size_t size);

/** A standard 32-voice bulk message (4104 bytes) for `voices`. */
std::array<uint8_t, bankBytes + 8> bankMessage(const uint8_t* voices);
/** A standard AMEM message (1128 bytes) for `amem`. */
std::array<uint8_t, amemBytes + 8> amemMessage(const uint8_t* amem);

/** Packs an unpacked voice (VCED order, 155 bytes) the way a 32-voice bulk
    stores it (128 bytes), as Cartridge::packProgram does with the operator
    switches all on and the name taken from the voice itself. */
std::array<uint8_t, 128> packVoice(const uint8_t* vced);

/** Decodes ASCII-hex nibbles ("0A" → 10), the encoding of the TX802
    performances and the fractional scaling. `highFirst` is the TX802 order. */
std::vector<uint8_t> decodeAsciiNibbles(const std::vector<uint8_t>& text, bool highFirst = true);
}
