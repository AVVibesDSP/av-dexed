// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedMicroTuning.h"
#include <cmath>
#include <string>

namespace AVDexedMicroTuning {
namespace {
/** Cents from equal temperament, C to B, of each 12-note preset (C = tonic). */
constexpr float twelveNote[8][12] = {
    // 0 Equal temperament
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    // 1 Pure (just) major: 1, 16/15, 9/8, 6/5, 5/4, 4/3, 45/32, 3/2, 8/5, 5/3, 9/5, 15/8
    { 0, 11.73f, 3.91f, 15.64f, -13.69f, -1.96f, -9.78f, 1.96f, 13.69f, -15.64f, 17.60f, -11.73f },
    // 2 Pure (just) minor: as major, with the minor seventh 16/9 and the major second 10/9
    { 0, 11.73f, -17.60f, 15.64f, -13.69f, -1.96f, -9.78f, 1.96f, 13.69f, -15.64f, -3.91f, -11.73f },
    // 3 Mean tone (quarter-comma)
    { 0, -24.04f, -6.84f, 10.26f, -13.69f, 3.42f, -20.53f, -3.42f, -27.37f, -10.26f, 6.84f, -17.11f },
    // 4 Pythagorean (wolf between G# and Eb)
    { 0, 13.69f, 3.91f, -5.87f, 7.82f, -1.96f, 11.73f, 1.96f, 15.64f, 5.87f, -3.91f, 9.78f },
    // 5 Werckmeister III
    { 0, -9.78f, -7.82f, -5.87f, -9.78f, -1.96f, -11.73f, -3.91f, -7.82f, -11.73f, -3.91f, -7.82f },
    // 6 Kirnberger III
    { 0, -9.78f, -6.84f, -5.87f, -13.69f, -1.96f, -9.78f, -3.42f, -7.82f, -10.26f, -3.91f, -11.73f },
    // 7 Vallotti (& Young)
    { 0, -5.87f, -3.91f, -1.96f, -7.82f, 1.96f, -7.82f, -1.96f, -3.91f, -5.87f, 0, -9.78f },
};

/** The engine's pitch units (an octave is 1 << 24), as StandardTuning counts them. */
struct TableTuning final : public TuningState {
    std::array<int32_t, 128> logfreq {};
    std::string name;
    int32_t midinote_to_logfreq(int midinote) override {
        return logfreq[size_t(midinote < 0 ? 0 : midinote > 127 ? 127 : midinote)];
    }
    bool is_standard_tuning() override { return false; }
    int scale_length() override { return 12; }
    std::string display_tuning_str() override { return name; }
};
}

const char* presetName(int table) {
    static const char* names[presetCount] = { "Equal", "Pure major", "Pure minor", "Mean tone", "Pythagorean",
                                              "Werckmeister", "Kirnberger", "Vallotti & Young", "1/4 shifted equal",
                                              "1/4 tone", "1/8 tone" };
    return table >= 0 && table < presetCount ? names[table] : "";
}

std::array<float, 128> presetCents(int table, int key) {
    std::array<float, 128> cents {};
    key = ((key % 12) + 12) % 12;
    for (int note = 0; note < 128; ++note) {
        if (table >= 0 && table < 8) {
            // Presets 1-4 follow the key; the well temperaments are on C.
            const int degree = table >= 1 && table <= 4 ? ((note - key) % 12 + 12) % 12 : note % 12;
            cents[size_t(note)] = twelveNote[table][degree];
        } else if (table == 8) {
            cents[size_t(note)] = 50.0f;                              // equal, a quarter tone up
        } else if (table == 9) {
            cents[size_t(note)] = float(note - 60) * (50.0f - 100.0f);   // a quarter tone per key
        } else if (table == 10) {
            cents[size_t(note)] = float(note - 60) * (25.0f - 100.0f);   // an eighth tone per key
        }
    }
    return cents;
}

std::shared_ptr<TuningState> makePresetTuning(int table, int key) {
    if (table <= 0 || table >= presetCount) return nullptr;
    auto tuning = std::make_shared<TableTuning>();
    const auto cents = presetCents(table, key);
    const int base = 50857777;          // StandardTuning: note 0
    const int step = (1 << 24) / 12;
    for (int note = 0; note < 128; ++note)
        tuning->logfreq[size_t(note)] = base + step * note + int32_t(std::lround(double(cents[size_t(note)]) * double(1 << 24) / 1200.0));
    tuning->name = std::string("DX7II ") + presetName(table);
    return tuning;
}
}
