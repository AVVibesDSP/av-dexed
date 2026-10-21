// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

// The engine's kernels against their original code, kept here as it was in
// 0.3.0 (only the tables are read from the engine): a change made to run
// faster (the branch-free sine) must give the very same samples, bit for bit.
// The same for each engine's render (which operators it computes, in which
// order, into which bus), and for Mark I's four voices at a time against one
// at a time (NEON and plain C++). And the free phase donor: advancing only the
// phases must leave them where computing whole blocks does.

#include "../source/ui/AVDexedTheme.h"
#include "../source/engine/EngineMkI.h"
#include "../source/engine/EngineOpl.h"
#include "../source/core/Program.h"
#include "../source/msfa/dx7note.h"
#include "../source/msfa/fm_op_kernel.h"
#include "../source/msfa/env.h"
#include "../source/msfa/exp2.h"
#include "../source/msfa/freqlut.h"
#include "../source/msfa/lfo.h"
#include "../source/msfa/pitchenv.h"
#include "../source/msfa/porta.h"
#include "../source/msfa/sin.h"
#include "../source/msfa/tuning.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

namespace {
namespace original {
const int32_t zeros[N] = {0};

// EngineMkI.cpp, 0.3.0.
const uint16_t NEGATIVE_BIT = 0x8000;
const uint16_t ENV_BITDEPTH = 14;
const uint16_t SINLOG_BITDEPTH = 10;
const uint16_t SINLOG_TABLESIZE = 1<<SINLOG_BITDEPTH;
const uint16_t ENV_MAX = 1<<ENV_BITDEPTH;

inline uint16_t sinLog(uint16_t phi) {
    const uint16_t* sinLogTable = EngineMkI::testSinLogTable();
    const uint16_t SINLOG_TABLEFILTER = SINLOG_TABLESIZE-1;
    const uint16_t index = (phi & SINLOG_TABLEFILTER);

    switch( ( phi & (SINLOG_TABLESIZE * 3) ) ) {
        case 0:
            return sinLogTable[index];
        case SINLOG_TABLESIZE:
            return sinLogTable[index ^ SINLOG_TABLEFILTER];
        case SINLOG_TABLESIZE * 2 :
            return sinLogTable[index] | NEGATIVE_BIT;
        default:
            return sinLogTable[index ^ SINLOG_TABLEFILTER] | NEGATIVE_BIT;
    }
}

inline int32_t mkiSin(int32_t phase, uint16_t env) {
    uint16_t expVal = sinLog(phase >> (22 - SINLOG_BITDEPTH)) + (env);
    const bool isSigned = expVal & NEGATIVE_BIT;
    expVal &= ~NEGATIVE_BIT;

    const uint16_t SINEXP_FILTER = 0x3FF;
    uint16_t result = 4096 + EngineMkI::testSinExpTable()[( expVal & SINEXP_FILTER ) ^ SINEXP_FILTER];
    result >>= ( expVal >> 10 ); // exp

    if( isSigned )
        return (-result - 1) << 13;
    else
        return result << 13;
}

void mkiCompute(int32_t *output, const int32_t *input, int32_t phase0, int32_t freq,
                int32_t gain1, int32_t gain2, bool add) {
    int32_t dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    const int32_t *adder = add ? output : zeros;
    for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t y = mkiSin((phase+input[i]), gain);
        output[i] = y + adder[i];
        phase += freq;
    }
}

void mkiComputePure(int32_t *output, int32_t phase0, int32_t freq, int32_t gain1, int32_t gain2, bool add) {
    int32_t dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    const int32_t *adder = add ? output : zeros;
    for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t y = mkiSin(phase , gain);
        output[i] = y + adder[i];
        phase += freq;
    }
}

void mkiComputeFb(int32_t *output, int32_t phase0, int32_t freq, int32_t gain1, int32_t gain2,
                  int32_t *fb_buf, int fb_shift, bool add) {
    int32_t dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    const int32_t *adder = add ? output : zeros;
    int32_t y0 = fb_buf[0];
    int32_t y = fb_buf[1];
    for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t scaled_fb = (y0 + y) >> (fb_shift + 1);
        y0 = y;
        y = mkiSin((phase+scaled_fb), gain);
        output[i] = y + adder[i];
        phase += freq;
    }
    fb_buf[0] = y0;
    fb_buf[1] = y;
}

void mkiComputeFb2(int32_t *output, FmOpParams *parms, int32_t gain01, int32_t gain02, int32_t *fb_buf, int fb_shift) {
    int32_t dgain[2];
    int32_t gain[2];
    int32_t phase[2];
    int32_t y0 = fb_buf[0];
    int32_t y = fb_buf[1];
    phase[0] = parms[0].phase;
    phase[1] = parms[1].phase;
    parms[1].gain_out = (ENV_MAX-(parms[1].level_in >> (28-ENV_BITDEPTH)));
    gain[0] = gain01;
    gain[1] = parms[1].gain_out == 0 ? (ENV_MAX-1) : parms[1].gain_out;
    dgain[0] = (gain02 - gain01 + (N >> 1)) >> LG_N;
    dgain[1] = (parms[1].gain_out - (parms[1].gain_out == 0 ? (ENV_MAX-1) : parms[1].gain_out));
    for (int i = 0; i < N; i++) {
        int32_t scaled_fb = (y0 + y) >> (fb_shift + 1);
        gain[0] += dgain[0];
        y0 = y;
        y = mkiSin(phase[0]+scaled_fb, gain[0]);
        phase[0] += parms[0].freq;
        gain[1] += dgain[1];
        y = mkiSin(phase[1]+y, gain[1]);
        phase[1] += parms[1].freq;
        output[i] = y;
    }
    fb_buf[0] = y0;
    fb_buf[1] = y;
}

void mkiComputeFb3(int32_t *output, FmOpParams *parms, int32_t gain01, int32_t gain02, int32_t *fb_buf, int fb_shift) {
    int32_t dgain[3];
    int32_t gain[3];
    int32_t phase[3];
    int32_t y0 = fb_buf[0];
    int32_t y = fb_buf[1];
    phase[0] = parms[0].phase;
    phase[1] = parms[1].phase;
    phase[2] = parms[2].phase;
    parms[1].gain_out = (ENV_MAX-(parms[1].level_in >> (28-ENV_BITDEPTH)));
    parms[2].gain_out = (ENV_MAX-(parms[2].level_in >> (28-ENV_BITDEPTH)));
    gain[0] = gain01;
    gain[1] = parms[1].gain_out == 0 ? (ENV_MAX-1) : parms[1].gain_out;
    gain[2] = parms[2].gain_out == 0 ? (ENV_MAX-1) : parms[2].gain_out;
    dgain[0] = (gain02 - gain01 + (N >> 1)) >> LG_N;
    dgain[1] = (parms[1].gain_out - (parms[1].gain_out == 0 ? (ENV_MAX-1) : parms[1].gain_out));
    dgain[2] = (parms[2].gain_out - (parms[2].gain_out == 0 ? (ENV_MAX-1) : parms[2].gain_out));
    for (int i = 0; i < N; i++) {
        int32_t scaled_fb = (y0 + y) >> (fb_shift + 1);
        gain[0] += dgain[0];
        y0 = y;
        y = mkiSin(phase[0]+scaled_fb, gain[0]);
        phase[0] += parms[0].freq;
        gain[1] += dgain[1];
        y = mkiSin(phase[1]+y, gain[1]);
        phase[1] += parms[1].freq;
        gain[2] += dgain[2];
        y = mkiSin(phase[2]+y, gain[2]);
        phase[2] += parms[2].freq;
        output[i] = y;
    }
    fb_buf[0] = y0;
    fb_buf[1] = y;
}

// EngineOpl.cpp, 0.3.0.
const uint16_t SignBit = 0x8000;

inline uint16_t oplSinLog(uint16_t phi) {
    const uint16_t* sinLogTable = EngineOpl::testSinLogTable();
    const uint8_t index = (phi & 0xff);

    switch( ( phi & 0x0300 ) ) {
        case 0x0000:
            return sinLogTable[index];
        case 0x0100:
            return sinLogTable[index ^ 0xFF];
        case 0x0200:
            return sinLogTable[index] | SignBit;
        default:
            return sinLogTable[index ^ 0xFF] | SignBit;
    }
}

inline int16_t oplSin( uint16_t phase, uint16_t env ) {
    uint16_t expVal = oplSinLog(phase) + (env << 3);
    const bool isSigned = expVal & SignBit;

    expVal &= ~SignBit;
    uint32_t result = 0x0400 + EngineOpl::testSinExpTable()[( expVal & 0xff ) ^ 0xFF];
    result <<= 1;
    result >>= ( expVal >> 8 ); // exp

    if( isSigned ) {
        return -result - 1;
    } else {
        return result;
    }
}

void oplCompute(int32_t *output, const int32_t *input, int32_t phase0, int32_t freq, int32_t gain1, int32_t gain2, bool add) {
    int32_t dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    const int32_t *adder = add ? output : zeros;
    for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t y = oplSin((phase+input[i]) >> 14, gain);
        output[i] = (y << 14) + adder[i];
        phase += freq;
    }
}

void oplComputePure(int32_t *output, int32_t phase0, int32_t freq, int32_t gain1, int32_t gain2, bool add) {
    int32_t dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    const int32_t *adder = add ? output : zeros;
    for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t y = oplSin(phase >> 14, gain);
        output[i] = (y << 14) + adder[i];
        phase += freq;
    }
}

void oplComputeFb(int32_t *output, int32_t phase0, int32_t freq, int32_t gain1, int32_t gain2,
                  int32_t *fb_buf, int fb_shift, bool add) {
    int32_t dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    const int32_t *adder = add ? output : zeros;
    int32_t y0 = fb_buf[0];
    int32_t y = fb_buf[1];
    for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t scaled_fb = (y0 + y) >> (fb_shift + 1);
        y0 = y;
        y = oplSin((phase+scaled_fb) >> 14, gain) << 14;
        output[i] = y + adder[i];
        phase += freq;
    }
    fb_buf[0] = y0;
    fb_buf[1] = y;
}

// The engines' render, 0.3.0: the same control flow, with the engines' own
// kernels (checked above) and buses of its own.
struct Buses { int32_t data[2][N]; int32_t* get(int bus) { return data[bus]; } };

void msfaRender(int32_t *output, FmOpParams *params, int algorithm, int32_t *fb_buf, int feedback_shift, Buses& buf_) {
    const int kLevelThresh = 1120;
    const FmAlgorithm alg = FmCore::testAlgorithm(algorithm);
    bool has_contents[3] = { true, false, false };
    for (int op = 0; op < 6; op++) {
        int flags = alg.ops[op];
        bool add = (flags & OUT_BUS_ADD) != 0;
        FmOpParams &param = params[op];
        int inbus = (flags >> 4) & 3;
        int outbus = flags & 3;
        int32_t *outptr = (outbus == 0) ? output : buf_.get(outbus - 1);
        int32_t gain1 = param.gain_out;
        int32_t gain2 = Exp2::lookup(param.level_in - (14 * (1 << 24)));
        param.gain_out = gain2;
        if (gain1 >= kLevelThresh || gain2 >= kLevelThresh) {
            if (!has_contents[outbus]) {
                add = false;
            }
            if (inbus == 0 || !has_contents[inbus]) {
                if ((flags & 0xc0) == 0xc0 && feedback_shift < 16) {
                    FmOpKernel::compute_fb(outptr, param.phase, param.freq, gain1, gain2, fb_buf, feedback_shift, add);
                } else {
                    FmOpKernel::compute_pure(outptr, param.phase, param.freq, gain1, gain2, add);
                }
            } else {
                FmOpKernel::compute(outptr, buf_.get(inbus - 1), param.phase, param.freq, gain1, gain2, add);
            }
            has_contents[outbus] = true;
        } else if (!add) {
            has_contents[outbus] = false;
        }
        param.phase += param.freq << LG_N;
    }
}

void mkiRender(EngineMkI& engine, int32_t *output, FmOpParams *params, int algorithm, int32_t *fb_buf, int32_t feedback_shift, Buses& buf_) {
    const int kLevelThresh = ENV_MAX-100;
    FmAlgorithm alg = FmCore::testAlgorithm(algorithm);
    bool has_contents[3] = { true, false, false };
    bool fb_on = feedback_shift < 16;
    switch(algorithm) {
        case 3 : case 5 :
            if ( fb_on )
                alg.ops[0] = 0xc4;
    }
    for (int op = 0; op < 6; op++) {
        int flags = alg.ops[op];
        bool add = (flags & OUT_BUS_ADD) != 0;
        FmOpParams &param = params[op];
        int inbus = (flags >> 4) & 3;
        int outbus = flags & 3;
        int32_t *outptr = (outbus == 0) ? output : buf_.get(outbus - 1);
        int32_t gain1 = param.gain_out == 0 ? (ENV_MAX-1) : param.gain_out;
        int32_t gain2 = ENV_MAX-(param.level_in >> (28-ENV_BITDEPTH));
        param.gain_out = gain2;
        if (gain1 <= kLevelThresh || gain2 <= kLevelThresh) {
            if (!has_contents[outbus]) {
                add = false;
            }
            if (inbus == 0 || !has_contents[inbus]) {
                if ((flags & 0xc0) == 0xc0 && fb_on) {
                    switch ( algorithm ) {
                        case 3 :
                            engine.compute_fb3(outptr, params, gain1, gain2, fb_buf, std::min((feedback_shift+2), 16));
                            params[1].phase += params[1].freq << LG_N;
                            params[2].phase += params[2].freq << LG_N;
                            op += 2;
                            break;
                        case 5 :
                            engine.compute_fb2(outptr, params, gain1, gain2, fb_buf, std::min((feedback_shift+2), 16));
                            params[1].phase += params[1].freq << LG_N;
                            op++;
                            break;
                        case 31 :
                            engine.compute_fb(outptr, param.phase, param.freq, gain1, gain2, fb_buf, std::min((feedback_shift+2), 16), add);
                            break;
                        default:
                            engine.compute_fb(outptr, param.phase, param.freq, gain1, gain2, fb_buf, feedback_shift, add);
                            break;
                    }
                } else {
                    engine.compute_pure(outptr, param.phase, param.freq, gain1, gain2, add);
                }
            } else {
                engine.compute(outptr, buf_.get(inbus - 1), param.phase, param.freq, gain1, gain2, add);
            }
            has_contents[outbus] = true;
        } else if (!add) {
            has_contents[outbus] = false;
        }
        param.phase += param.freq << LG_N;
    }
}

void oplRender(EngineOpl& engine, int32_t *output, FmOpParams *params, int algorithm, int32_t *fb_buf, int32_t feedback_shift, Buses& buf_) {
    const int kLevelThresh = 507;
    const FmAlgorithm alg = FmCore::testAlgorithm(algorithm);
    bool has_contents[3] = { true, false, false };
    for (int op = 0; op < 6; op++) {
        int flags = alg.ops[op];
        bool add = (flags & OUT_BUS_ADD) != 0;
        FmOpParams &param = params[op];
        int inbus = (flags >> 4) & 3;
        int outbus = flags & 3;
        int32_t *outptr = (outbus == 0) ? output : buf_.get(outbus - 1);
        int32_t gain1 = param.gain_out == 0 ? 511 : param.gain_out;
        int32_t gain2 = 512-(param.level_in >> 19);
        param.gain_out = gain2;
        if (gain1 <= kLevelThresh || gain2 <= kLevelThresh) {
            if (!has_contents[outbus]) {
                add = false;
            }
            if (inbus == 0 || !has_contents[inbus]) {
                if ((flags & 0xc0) == 0xc0 && feedback_shift < 16) {
                    engine.compute_fb(outptr, param.phase, param.freq, gain1, gain2, fb_buf, feedback_shift, add);
                } else {
                    engine.compute_pure(outptr, param.phase, param.freq, gain1, gain2, add);
                }
            } else {
                engine.compute(outptr, buf_.get(inbus - 1), param.phase, param.freq, gain1, gain2, add);
            }
            has_contents[outbus] = true;
        } else if (!add) {
            has_contents[outbus] = false;
        }
        param.phase += param.freq << LG_N;
    }
}
}

struct Blocks {
    int32_t expected[N], actual[N];
    void fill(std::mt19937& random) {
        for (int i = 0; i < N; ++i) expected[i] = actual[i] = int32_t(random());
    }
    bool same() const { return std::memcmp(expected, actual, sizeof(expected)) == 0; }
};

bool report(const char* what, long mismatches, long total) {
    if (mismatches == 0) return true;
    std::cerr << "Kernel " << what << ": " << mismatches << " of " << total << " blocks differ from the original code\n";
    return false;
}
}

bool runAVDexedKernelTests() {
    EngineMkI mki;   // builds its tables
    EngineOpl opl;
    std::mt19937 random(20261021);
    bool passed = true;

    // Every step of the sine table, in both halves of every quarter, at a range
    // of levels: one table step per sample, 1024 blocks per level.
    {
        long mismatches = 0, total = 0;
        std::vector<int32_t> levels { 0, 1, 2, 100, original::ENV_MAX - 100, original::ENV_MAX - 1, original::ENV_MAX, 20000, 65535 };
        for (int32_t level = 0; level < original::ENV_MAX; level += 160) levels.push_back(level);
        for (const auto level : levels)
            for (int block = 0; block < 1024; ++block) {
                Blocks b;
                const int32_t phase0 = int32_t(uint32_t(block * N) << 12);
                original::mkiComputePure(b.expected, phase0, 1 << 12, level, level, false);
                mki.compute_pure(b.actual, phase0, 1 << 12, level, level, false);
                mismatches += b.same() ? 0 : 1;
                ++total;
            }
        passed &= report("Mark I sine, every table step", mismatches, total);
    }
    {
        long mismatches = 0, total = 0;
        for (int32_t level = 0; level <= 520; level += 4)
            for (int block = 0; block < 1024; ++block) {
                Blocks b;
                const int32_t phase0 = int32_t(uint32_t(block * N) << 14);
                original::oplComputePure(b.expected, phase0, 1 << 14, level, level, false);
                opl.compute_pure(b.actual, phase0, 1 << 14, level, level, false);
                mismatches += b.same() ? 0 : 1;
                ++total;
            }
        passed &= report("OPL sine, every table step", mismatches, total);
    }

    // Random kernels: modulated, pure, feedback (and Mark I's 2- and 3-operator
    // feedback chains), with and without adding to the bus.
    const auto randomGain = [&random](int32_t top) { return int32_t(random() % uint32_t(top + 1)); };
    long mkiMismatches = 0, oplMismatches = 0;
    constexpr long kernels = 20000;
    for (long k = 0; k < kernels; ++k) {
        int32_t input[N];
        for (auto& sample : input) sample = int32_t(random());
        const int32_t phase0 = int32_t(random()), freq = int32_t(random() >> (random() % 12));
        const bool add = (random() & 1) != 0;
        const int fbShift = int(random() % 17);
        {
            const int32_t gain1 = randomGain(original::ENV_MAX), gain2 = randomGain(original::ENV_MAX);
            Blocks b;
            b.fill(random);
            original::mkiCompute(b.expected, input, phase0, freq, gain1, gain2, add);
            mki.compute(b.actual, input, phase0, freq, gain1, gain2, add);
            bool same = b.same();
            b.fill(random);
            original::mkiComputePure(b.expected, phase0, freq, gain1, gain2, add);
            mki.compute_pure(b.actual, phase0, freq, gain1, gain2, add);
            same &= b.same();
            int32_t fbExpected[2] = { int32_t(random()) >> 3, int32_t(random()) >> 3 }, fbActual[2] = { fbExpected[0], fbExpected[1] };
            b.fill(random);
            original::mkiComputeFb(b.expected, phase0, freq, gain1, gain2, fbExpected, fbShift, add);
            mki.compute_fb(b.actual, phase0, freq, gain1, gain2, fbActual, fbShift, add);
            same &= b.same() && fbExpected[0] == fbActual[0] && fbExpected[1] == fbActual[1];
            for (int chain : { 2, 3 }) {
                FmOpParams expectedParams[3], actualParams[3];
                for (auto& p : expectedParams)
                    p = { int32_t(random() >> 3), randomGain(original::ENV_MAX), int32_t(random() >> 4), int32_t(random()) };
                std::memcpy(actualParams, expectedParams, sizeof(expectedParams));
                b.fill(random);
                fbActual[0] = fbExpected[0];
                fbActual[1] = fbExpected[1];
                if (chain == 2) {
                    original::mkiComputeFb2(b.expected, expectedParams, gain1, gain2, fbExpected, fbShift);
                    mki.compute_fb2(b.actual, actualParams, gain1, gain2, fbActual, fbShift);
                } else {
                    original::mkiComputeFb3(b.expected, expectedParams, gain1, gain2, fbExpected, fbShift);
                    mki.compute_fb3(b.actual, actualParams, gain1, gain2, fbActual, fbShift);
                }
                same &= b.same() && fbExpected[0] == fbActual[0] && fbExpected[1] == fbActual[1]
                     && std::memcmp(expectedParams, actualParams, sizeof(expectedParams)) == 0;
            }
            mkiMismatches += same ? 0 : 1;
        }
        {
            const int32_t gain1 = randomGain(512), gain2 = randomGain(512);
            Blocks b;
            b.fill(random);
            original::oplCompute(b.expected, input, phase0, freq, gain1, gain2, add);
            opl.compute(b.actual, input, phase0, freq, gain1, gain2, add);
            bool same = b.same();
            b.fill(random);
            original::oplComputePure(b.expected, phase0, freq, gain1, gain2, add);
            opl.compute_pure(b.actual, phase0, freq, gain1, gain2, add);
            same &= b.same();
            int32_t fbExpected[2] = { int32_t(random()) >> 3, int32_t(random()) >> 3 }, fbActual[2] = { fbExpected[0], fbExpected[1] };
            b.fill(random);
            original::oplComputeFb(b.expected, phase0, freq, gain1, gain2, fbExpected, fbShift, add);
            opl.compute_fb(b.actual, phase0, freq, gain1, gain2, fbActual, fbShift, add);
            same &= b.same() && fbExpected[0] == fbActual[0] && fbExpected[1] == fbActual[1];
            oplMismatches += same ? 0 : 1;
        }
    }
    passed &= report("Mark I, random kernels", mkiMismatches, kernels);
    passed &= report("OPL, random kernels", oplMismatches, kernels);

    // Whole renders, eight blocks in a row (feedback memory and gains carry on),
    // every algorithm and feedback; in half the cases every operator that
    // writes to the output is silent (its gains past the threshold) while the
    // others sound, as in a voice whose carriers have ended.
    {
        FmCore msfa;
        long mismatches = 0, total = 0;
        for (int engineIndex = 0; engineIndex < 3; ++engineIndex)
            for (int round = 0; round < 1500; ++round) {
                const int algorithm = int(random() % 32);
                const int feedbackShift = int(random() % 17);
                const bool carriersSilent = (round & 1) != 0;
                FmOpParams expectedParams[6], actualParams[6];
                for (int op = 0; op < 6; ++op) {
                    const bool carrier = (FmCore::testAlgorithm(algorithm).ops[op] & 3) == 0;
                    const bool silent = carrier && carriersSilent;
                    auto& p = expectedParams[op];
                    p.freq = int32_t(random() >> (4 + random() % 8));
                    p.phase = int32_t(random());
                    if (engineIndex == 0) {        // MSFA: louder gains are larger
                        p.level_in = silent ? int32_t(random() % (8u << 24)) : int32_t((14u << 24) + random() % (6u << 24));
                        p.gain_out = silent ? int32_t(random() % 1000) : int32_t(random() % (1 << 24));
                    } else if (engineIndex == 1) { // Mark I: louder gains are smaller
                        p.level_in = silent ? int32_t(random() % (90u << 14)) : int32_t(random() % (1u << 28));
                        p.gain_out = silent ? original::ENV_MAX - 50 : randomGain(original::ENV_MAX);
                    } else {                       // OPL: louder gains are smaller
                        p.level_in = silent ? int32_t(random() % (4u << 19)) : int32_t(random() % (1u << 28));
                        p.gain_out = silent ? 510 : randomGain(512);
                    }
                }
                std::memcpy(actualParams, expectedParams, sizeof(expectedParams));
                int32_t fbExpected[2] = { int32_t(random()) >> 3, int32_t(random()) >> 3 }, fbActual[2] = { fbExpected[0], fbExpected[1] };
                original::Buses buses;
                bool same = true;
                for (int block = 0; block < 8 && same; ++block) {
                    Blocks b;
                    b.fill(random);
                    if (engineIndex == 0) {
                        original::msfaRender(b.expected, expectedParams, algorithm, fbExpected, feedbackShift, buses);
                        msfa.render(b.actual, actualParams, algorithm, fbActual, feedbackShift);
                    } else if (engineIndex == 1) {
                        original::mkiRender(mki, b.expected, expectedParams, algorithm, fbExpected, feedbackShift, buses);
                        mki.render(b.actual, actualParams, algorithm, fbActual, feedbackShift);
                    } else {
                        original::oplRender(opl, b.expected, expectedParams, algorithm, fbExpected, feedbackShift, buses);
                        opl.render(b.actual, actualParams, algorithm, fbActual, feedbackShift);
                    }
                    same = b.same() && fbExpected[0] == fbActual[0] && fbExpected[1] == fbActual[1]
                        && std::memcmp(expectedParams, actualParams, sizeof(expectedParams)) == 0;
                }
                mismatches += same ? 0 : 1;
                ++total;
            }
        passed &= report("engine render (8 blocks)", mismatches, total);
    }

    // Mark I, four and eight voices at a time (renderFour, renderEight) against
    // each alone (render): one to eight voices, each with its own gains (some
    // silent, some sounding), phases and feedback memory, eight blocks in a
    // row; with NEON and in plain C++ (what x86 runs).
    const auto voicesAtATime = [&](auto width, bool simd) {
        constexpr int W = decltype(width)::value;
        EngineMkI::useSimd = simd;
        long mismatches = 0, total = 0;
        for (int round = 0; round < 2000; ++round) {
            int algorithm = int(random() % 32);
            const int feedbackShift = int(random() % 17);
            if (!EngineMkI::canRenderFour(algorithm, feedbackShift)) algorithm = 0;
            const int lanes = 1 + int(random() % W);
            FmOpParams alone[W][6], together[W][6];
            int32_t fbAlone[W][2], fbTogether[W][2];
            for (int l = 0; l < lanes; ++l) {
                const int kind = int(random() % 3);   // sounding, every carrier silent, anything
                for (int op = 0; op < 6; ++op) {
                    const bool carrier = (FmCore::testAlgorithm(algorithm).ops[op] & 3) == 0;
                    const bool silent = (kind == 1 && carrier) || (kind == 2 && random() % 3 == 0);
                    auto& p = alone[l][op];
                    p.freq = int32_t(random() >> (4 + random() % 8));
                    p.phase = int32_t(random());
                    p.level_in = silent ? int32_t(random() % (90u << 14)) : int32_t(random() % (1u << 28));
                    p.gain_out = silent ? original::ENV_MAX - 50 : randomGain(original::ENV_MAX);
                }
                fbAlone[l][0] = int32_t(random()) >> 3;
                fbAlone[l][1] = int32_t(random()) >> 3;
            }
            std::memcpy(together, alone, sizeof(FmOpParams) * 6 * size_t(lanes));
            std::memcpy(fbTogether, fbAlone, sizeof(int32_t) * 2 * size_t(lanes));
            bool same = true;
            for (int block = 0; block < 8 && same; ++block) {
                int32_t expected[W][N] = {}, actual[W][N] = {};
                FmOpParams* params[W] = {};
                int32_t* feedback[W] = {};
                for (int l = 0; l < lanes; ++l) {
                    params[l] = together[l];
                    feedback[l] = fbTogether[l];
                    mki.render(expected[l], alone[l], algorithm, fbAlone[l], feedbackShift);   // each alone, into zeros
                }
                alignas(16) int32_t output[N][W];
                int wrote;
                if constexpr (W == 4) wrote = mki.renderFour(output, params, feedback, lanes, algorithm, feedbackShift);
                else wrote = mki.renderEight(output, params, feedback, lanes, algorithm, feedbackShift);
                for (int l = 0; l < lanes; ++l) {
                    for (int j = 0; j < N; ++j) actual[l][j] = output[j][l];
                    // A lane said to have written nothing must hold zeros.
                    if ((wrote & (1 << l)) == 0)
                        for (int j = 0; j < N; ++j) same &= expected[l][j] == 0;
                }
                same = same && std::memcmp(expected, actual, sizeof(int32_t) * N * size_t(lanes)) == 0
                    && std::memcmp(alone, together, sizeof(FmOpParams) * 6 * size_t(lanes)) == 0
                    && std::memcmp(fbAlone, fbTogether, sizeof(int32_t) * 2 * size_t(lanes)) == 0;
            }
            mismatches += same ? 0 : 1;
            ++total;
        }
        const std::string what = std::string("Mark I, ") + (W == 4 ? "four" : "eight") + " voices at a time ("
                               + (simd ? "NEON where there is" : "plain C++") + ")";
        passed &= report(what.c_str(), mismatches, total);
    };
    for (const bool simd : { true, false }) {
        voicesAtATime(std::integral_constant<int, 4>(), simd);
        voicesAtATime(std::integral_constant<int, 8>(), simd);
    }
    EngineMkI::useSimd = true;

    // The free phase donor (AVDexedLayer::startFreePhase): N whole blocks
    // against N blocks of advancePhases, as the layer tunes it, over varied
    // voices (fixed and ratio frequencies, pitch envelopes, switched-off
    // operators), every engine, keys from A0 to C8 and four amounts of turn.
    {
        const double rate = 48000.0;
        Exp2::init(); Tanh::init(); Sin::init();
        Freqlut::init(rate); Lfo::init(rate); PitchEnv::init(rate); Env::init_sr(rate); Porta::init_sr(rate);
        FmCore msfa;
        FmCore* engines[] { &msfa, &mki, &opl };
        auto tuning = createStandardTuning();
        long mismatches = 0, compared = 0;
        for (int voiceIndex = 0; voiceIndex < 48; ++voiceIndex) {
            uint8_t patch[Program::PROGRAM_SIZE + 1];
            std::memcpy(patch, Program::initialProgram().getRawData(), sizeof(patch));
            for (int op = 0; op < 6; ++op) {
                patch[op * 21 + 16] = uint8_t(random() % 100);   // output level
                patch[op * 21 + 17] = uint8_t(random() % 4 == 0); // fixed frequency
                patch[op * 21 + 18] = uint8_t(random() % 32);    // coarse
                patch[op * 21 + 19] = uint8_t(random() % 100);   // fine
                patch[op * 21 + 20] = uint8_t(random() % 15);    // detune
            }
            for (int i = 126; i < 134; ++i) patch[i] = uint8_t(random() % 100);   // pitch EG rates and levels
            patch[134] = uint8_t(random() % 32);                                   // algorithm
            patch[135] = uint8_t(random() % 8);                                    // feedback
            patch[155] = uint8_t(random() % 4 == 0 ? random() % 64 : 63);          // operators on
            for (auto* engine : engines) {
                Controllers still;
                still.pitch_cc = 0x2000; still.pitch_range_up = 3; still.pitch_range_dn = 3; still.pitch_step = 0;
                still.aftertouch_cc = still.breath_cc = still.foot_cc = still.modwheel_cc = 0;
                still.portamento_enable_cc = false; still.portamento_cc = 0; still.portamento_gliss_cc = false;
                still.masterTune = 0; still.mpeEnabled = false; still.core = engine;
                still.refresh();
                for (int pitch = 21; pitch <= 108; pitch += 5)
                    for (const float cycles : { 0.05f, 0.3f, 0.61f, 0.97f }) {
                        Dx7Note whole(tuning, nullptr), phasesOnly(tuning, nullptr);
                        whole.init(patch, pitch, 100, 1, &still);
                        whole.oscSync();
                        phasesOnly.init(patch, pitch, 100, 1, &still);
                        phasesOnly.oscSync();
                        // As startFreePhase tunes its donor.
                        const int32_t noteLogfreq = tuning->midinote_to_logfreq(pitch);
                        const double noteHz = std::exp2(double(noteLogfreq) / double(1 << 24));
                        const int blocks = std::clamp(int(std::lround(double(cycles) * rate / (double(N) * noteHz))), 1, 16);
                        const double hz = double(cycles) * rate / (double(N) * double(blocks));
                        Controllers tuned = still;
                        tuned.masterTune = int32_t(std::lround(std::log2(hz) * double(1 << 24))) - noteLogfreq;
                        int32_t scratch[N];
                        for (int block = 0; block < blocks; ++block) {
                            std::fill(scratch, scratch + N, 0);
                            whole.compute(scratch, 0, 0, &tuned);
                            phasesOnly.advancePhases(&tuned);
                        }
                        for (int op = 0; op < 6; ++op) {
                            mismatches += whole.testPhase(op) != phasesOnly.testPhase(op) ? 1 : 0;
                            ++compared;
                        }
                    }
            }
        }
        if (mismatches != 0)
            std::cerr << "Free phase donor: " << mismatches << " of " << compared << " phases differ from whole blocks\n";
        passed &= mismatches == 0;
    }

    // The dialogs' backdrop blur (AVDexedTheme::gaussianBlur9) against JUCE's
    // ImageConvolutionKernel it replaces: within one level everywhere,
    // including the edges, for 24- and 32-bit pictures; and its time.
    {
        juce::Random random(7);
        int over = 0, off = 0, total = 0;
        double ours = 0.0, juces = 0.0;
        for (const auto format : { juce::Image::RGB, juce::Image::ARGB })
            for (const auto size : { juce::Point<int>(480, 313), juce::Point<int>(7, 5), juce::Point<int>(1, 12) }) {
                juce::Image picture(format, size.x, size.y, true, juce::SoftwareImageType());
                for (int y = 0; y < size.y; ++y)
                    for (int x = 0; x < size.x; ++x) {
                        const auto noise = (juce::uint8) random.nextInt(256);
                        const auto ramp = (juce::uint8) ((x * 255) / juce::jmax(1, size.x - 1));
                        picture.setPixelAt(x, y, juce::Colour::fromRGBA(noise, ramp, (juce::uint8) (noise ^ ramp), (juce::uint8) random.nextInt(256)));
                    }
                auto reference = picture.createCopy();
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                juce::ImageConvolutionKernel kernel(9);
                kernel.createGaussianBlur(2.0f);
                kernel.applyToImage(reference, picture, picture.getBounds());
                const double t1 = juce::Time::getMillisecondCounterHiRes();
                const auto fast = AVDexedTheme::gaussianBlur9(picture);
                const double t2 = juce::Time::getMillisecondCounterHiRes();
                juces += t1 - t0;
                ours += t2 - t1;
                const juce::Image::BitmapData a(reference, juce::Image::BitmapData::readOnly), b(fast, juce::Image::BitmapData::readOnly);
                for (int y = 0; y < size.y; ++y)
                    for (int x = 0; x < size.x; ++x)
                        for (int c = 0; c < a.pixelStride; ++c) {
                            const int d = std::abs(int(a.getPixelPointer(x, y)[c]) - int(b.getPixelPointer(x, y)[c]));
                            over += d > 1 ? 1 : 0;
                            off += d == 1 ? 1 : 0;
                            ++total;
                        }
            }
        std::cout << "Backdrop blur: " << off << " of " << total << " values one level off, " << over
                  << " more (two passes " << juce::String(ours, 1) << " ms, JUCE's kernel " << juce::String(juces, 1) << " ms)\n";
        passed &= over == 0 && off * 1000 < total;
    }

    std::cout << (passed ? "Kernel checks passed.\n" : "Kernel checks FAILED.\n");
    return passed;
}
