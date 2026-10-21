// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedVoiceBlock.h"
#include "EngineMkI.h"
#include "../msfa/aligned_buf.h"
#include "../msfa/dx7note.h"
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace {
// A voice's sample as the instrument has always mixed it: >> 4, clipped to
// 16 bits (a large negative goes to +1, as Dexed does), in 1/32768ths.
inline int clipped(int32_t sample) {
    const int32_t val = sample >> 4;
    return val < -(1 << 24) ? 0x8000 : val >= (1 << 24) ? 0x7fff : val >> 9;
}

void addClipped(const int32_t *voice, int32_t *sum) {
    for (int j = 0; j < N; ++j) sum[j] += clipped(voice[j]);
}

// L voices side by side (4 or 8), clipped as above and added to `sum`
// (integers: any order gives the same). A voice that wrote nothing holds
// zeros, which add zero.
template <int L>
void addClippedLanes(const int32_t (&voices)[N][L], int32_t *sum) {
#if defined(__ARM_NEON)
    const int32x4_t low = vdupq_n_s32(-(1 << 24)), high = vdupq_n_s32(1 << 24);
    const int32x4_t negativeClip = vdupq_n_s32(0x8000), positiveClip = vdupq_n_s32(0x7fff);
    const auto clip = [&](const int32_t *four) {
        const int32x4_t val = vshrq_n_s32(vld1q_s32(four), 4);
        return vbslq_s32(vcltq_s32(val, low), negativeClip, vbslq_s32(vcgeq_s32(val, high), positiveClip, vshrq_n_s32(val, 9)));
    };
    for (int j = 0; j < N; ++j) {
        int32x4_t total = clip(voices[j]);
        if constexpr (L == 8) total = vaddq_s32(total, clip(voices[j] + 4));
        sum[j] += vaddvq_s32(total);
    }
#else
    for (int j = 0; j < N; ++j)
        for (int l = 0; l < L; ++l) sum[j] += clipped(voices[j][l]);
#endif
}
}

void AVRenderVoiceBlock(Dx7Note *const *notes, int count, FmCore *core, int32_t *sum) {
    AlignedBuf<int32_t, N> one;
    const auto alone = [&](Dx7Note &note) {
        for (int j = 0; j < N; ++j) one.get()[j] = 0;
        note.render(one.get(), core);
        addClipped(one.get(), sum);
    };
    constexpr int mostVoices = 64;   // a layer or the simple path holds 16
    auto *markI = count <= mostVoices ? dynamic_cast<EngineMkI *>(core) : nullptr;
    if (markI == nullptr) {
        for (int v = 0; v < count; ++v) alone(*notes[v]);
        return;
    }
    // Voices that compute nothing this block (an ended voice whose operators
    // are all below the threshold) only keep their gains and phases: one by
    // one, and they add nothing. The others go up to eight at a time with
    // voices doing the same work (sound, or only the feedback operator), so a
    // batch is not one sounding voice beside idle ones.
    EngineMkI::Work work[mostVoices];
    bool done[mostVoices] = {};
    for (int v = 0; v < count; ++v) {
        Dx7Note &note = *notes[v];
        if (!EngineMkI::canRenderFour(note.algorithmIndex(), note.feedbackShift())) {
            alone(note);
            done[v] = true;
            continue;
        }
        work[v] = EngineMkI::workFor(note.operatorParams(), note.algorithmIndex());
        if (work[v] == EngineMkI::Work::nothing) {
            note.render(one.get(), core);   // writes nothing: an operator below the threshold is not computed
            done[v] = true;
        }
    }
    for (int first = 0; first < count; ++first) {
        if (done[first]) continue;
        const int algorithm = notes[first]->algorithmIndex();
        const int32_t feedbackShift = notes[first]->feedbackShift();
        // This voice and the next ones that share its algorithm, feedback and work.
        FmOpParams *params[8] = {};
        int32_t *feedback[8] = {};
        int taken = 0;
        for (int v = first; v < count && taken < 8; ++v) {
            if (done[v] || work[v] != work[first] || notes[v]->algorithmIndex() != algorithm
                || notes[v]->feedbackShift() != feedbackShift)
                continue;
            params[taken] = notes[v]->operatorParams();
            feedback[taken] = notes[v]->feedbackMemory();
            done[v] = true;
            ++taken;
        }
        // Up to four: four lanes (no idle feedback chains beside them).
        if (taken <= 4) {
            alignas(16) EngineMkI::FourVoices output;
            if (markI->renderFour(output, params, feedback, taken, algorithm, feedbackShift) != 0)
                addClippedLanes<4>(output, sum);
        } else {
            alignas(16) EngineMkI::EightVoices output;
            if (markI->renderEight(output, params, feedback, taken, algorithm, feedbackShift) != 0)
                addClippedLanes<8>(output, sum);
        }
    }
}
