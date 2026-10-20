// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../msfa/tuning.h"
#include <array>
#include <memory>

/** The eleven preset micro tunings of the DX7II and DX7s (the DX7s manual
    lists them in this order), as tables the engine already plays through its
    TuningState: nothing of the engine changes. Their cents are the standard
    historical values (Yamaha does not publish its own tables, so a note may
    differ from the instrument by a cent or two); the quarter- and eighth-tone
    tables keep middle C (60) where it is. In tables 1-4 the tonic follows
    the key (0 = C). */
namespace AVDexedMicroTuning {

constexpr int presetCount = 11;

/** "Equal", "Pure major", ... (English; the editor translates). */
const char* presetName(int table);

/** Cents away from equal temperament for every MIDI note. */
std::array<float, 128> presetCents(int table, int key);

/** A tuning the engine plays from those cents (nullptr for equal temperament
    or a table outside 0-10: the instrument's own tuning stays). */
std::shared_ptr<TuningState> makePresetTuning(int table, int key);
}
