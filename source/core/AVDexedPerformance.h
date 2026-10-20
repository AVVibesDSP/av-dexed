// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_core/juce_core.h>
#include "AVDexedSysex.h"
#include "Program.h"
#include <array>
#include <optional>
#include <vector>

/** Performances of the DX7II (single, dual, split) and the TX802 (eight tone
    generators), read into what the layer mode plays: which voice each layer
    plays and with which channel, keys, transposition, detune, volume and pan.
    Only reads data; the voices themselves come from the banks of the same
    file or of the files next to it (VoicePool).

    Formats: Yamaha's DX7IIFD/D "MIDI Technical Data" (PCED/PMEM, 51 bytes) and
    "TX802 MIDI System Exclusive Messages" (PMEM, 84 bytes in ASCII nibbles),
    checked against the factory files. */
namespace AVDexedPerformance {

constexpr int layerCount = 8;

/** Where a performance's voice number points. */
struct VoiceRef {
    enum class Source { none, internal, cartridge, presetA, presetB };
    Source source = Source::none;
    int number = 0;   // 0-63 within its source
    juce::String label() const;   // "I05", "C12", "A15", "B64" (1-based, as the instruments show them)
};

struct LayerPlan {
    bool used = false;          // the layer takes part in the performance
    bool enabled = false;       // and sounds (the TX802 can route a tone generator nowhere)
    VoiceRef voice;
    int channel = 0;            // 0: every channel
    int lowNote = 0, highNote = 127;
    int transpose = 0;          // semitones
    float detune = 0.0f;        // cents
    float volume = 1.0f;        // 0..1
    float pan = 0.5f;
    int alternateGroup = -1;    // TX802 alternate assign: the layers of one group take turns (-1: none)
};

struct Performance {
    enum class Kind { dx7ii, tx802 };
    Kind kind = Kind::dx7ii;
    juce::String name;
    int number = 0;             // 1-based position in its dump
    std::array<LayerPlan, layerCount> layers {};
    juce::StringArray notes;    // what is approximate or left out ("dynamic pan", "micro tuning")
};

/** Every performance in `contents` (DX7II PMEM/PCED, TX802 PMEM/PCED), in order. */
std::vector<Performance> readPerformances(const AVDexedSysex::Contents& contents);

/** Decoders, exposed for the tests. */
Performance fromDx7ii(const uint8_t* pced51, int number);
Performance fromTx802(const uint8_t* pmem84, int number);

/** The banks a performance's voice numbers point into: the internal memory
    (two banks: 1-32, 33-64) and the TX802's preset banks A and B (two banks
    each). Taken from the performance file itself when it holds banks, else
    from the files next to it whose names match. */
class VoicePool {
public:
    /** Builds the pool for `performanceFile` (its bytes already scanned as `contents`). */
    VoicePool(const juce::File& performanceFile, const AVDexedSysex::Contents& contents);
    /** A pool from explicit banks (tests). */
    VoicePool() = default;

    void setBank(VoiceRef::Source source, int half, const std::array<uint8_t, AVDexedSysex::bankBytes>& voices,
                 const juce::String& from);
    std::optional<Program> voiceFor(const VoiceRef& ref) const;
    /** The DX7II additional data (35 packed AMEM bytes) of that voice, when its bank had it. */
    std::optional<std::array<uint8_t, AVDexedSysex::amemVoiceBytes>> extrasFor(const VoiceRef& ref) const;
    /** The files the voices came from, for the notice. */
    juce::StringArray sources() const;

private:
    struct Half {
        bool present = false;
        std::array<uint8_t, AVDexedSysex::bankBytes> voices {};
        bool hasAmem = false;
        std::array<uint8_t, AVDexedSysex::amemBytes> amem {};
        juce::String from;
    };
    void setBank(VoiceRef::Source source, int half, const AVDexedSysex::Bank& bank, const juce::String& from);
    struct Sibling { juce::File file; juce::String name; std::vector<AVDexedSysex::Bank> banks; };
    /** The banks of the .syx files in `folder`; with `presetsOnly`, only
        those named like TX802 presets (…A1/A2/B1/B2), not read otherwise. */
    static std::vector<Sibling> readSiblings(const juce::File& folder, const juce::File& except, bool presetsOnly = false);
    std::array<std::array<Half, 2>, 5> banks {};   // by Source, then 1-32 / 33-64
};

/** A performance with its voices resolved: what the processor installs. */
struct ResolvedLayer {
    LayerPlan plan;
    Program voice = Program::initialProgram();
    bool voiceFound = false;
    // From the voice's DX7II additional data (none: the DX7's behaviour).
    float unison = 0.0f;        // cents up and down for the two voices of a key
    float randomPitch = 0.0f;   // cents
    int mono = -1;              // -1: as the instrument, 1: mono
};
/** Unison, random pitch and mono of one voice's AMEM (35 bytes). */
void applyVoiceExtras(ResolvedLayer& layer, const uint8_t* amem35);
std::array<ResolvedLayer, layerCount> resolve(const Performance& performance, const VoicePool& pool);
}
