/*
 * Copyright (C) 2015-2017 Pascal Gauthier.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 * The code is based on ppplay https://github.com/stohrendorf/ppplay and opl3
 * math documentation :
 * https://github.com/gtaylormb/opl3_fpga/blob/master/docs/opl3math/opl3math.pdf
 *
 */

#include "EngineMkI.h"
#include <cmath>
#include <cstdlib>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "msfa/sin.h"
#include "msfa/exp2.h"

#ifdef DEBUG
    #include "time.h"
    //#define MKIDEBUG
#endif

#ifdef _WIN32
#if _MSC_VER < 1800
    double log2(double n)  {  
        return log(n) / log(2.0);  
    }
    double round(double n) {
        return n < 0.0 ? ceil(n - 0.5) : floor(n + 0.5);
    }
#endif
    __declspec(align(16)) const int zeros[N] = {0};
#else
    const int32_t __attribute__ ((aligned(16))) zeros[N] = {0};
#endif

static const uint16_t NEGATIVE_BIT = 0x8000;
static const uint16_t ENV_BITDEPTH = 14;

static const uint16_t SINLOG_BITDEPTH = 10;
static const uint16_t SINLOG_TABLESIZE = 1<<SINLOG_BITDEPTH;
static uint16_t sinLogTable[SINLOG_TABLESIZE];

static const uint16_t SINEXP_BITDEPTH = 10;
static const uint16_t SINEXP_TABLESIZE = 1<<SINEXP_BITDEPTH;
static uint16_t sinExpTable[SINEXP_TABLESIZE];

const uint16_t ENV_MAX = 1<<ENV_BITDEPTH;

// The quarter of the wave as masks, not a switch: with FM the quarter changes
// from sample to sample and the switch was a branch the processor mispredicted
// (a modulated operator took 1.57 times as long). Mirrored in the 2nd and 4th
// quarters, negative in the 3rd and 4th: the same table entries and bits.
static inline uint16_t sinLog(uint16_t phi) {
    const uint32_t SINLOG_TABLEFILTER = SINLOG_TABLESIZE-1;
    const uint32_t quarter = phi >> SINLOG_BITDEPTH;
    const uint32_t index = (phi & SINLOG_TABLEFILTER) ^ ((0u - (quarter & 1u)) & SINLOG_TABLEFILTER);
    return uint16_t(sinLogTable[index] | ((0u - ((quarter >> 1) & 1u)) & NEGATIVE_BIT));
}

EngineMkI::EngineMkI() {
    float bitReso = SINLOG_TABLESIZE;
    
    for(int i=0;i<SINLOG_TABLESIZE;i++) {
        float x1 = sin(((0.5+i)/bitReso) * M_PI/2.0);
        sinLogTable[i] = round(-1024 * log2(x1));
    }
    
    bitReso = SINEXP_TABLESIZE;
    for(int i=0;i<SINEXP_TABLESIZE;i++) {
        float x1 = (pow(2, float(i)/bitReso)-1) * 4096;
        sinExpTable[i] = round(x1);
    }
    
#ifdef MKIDEBUG
    char buffer[4096];
    int pos = 0;
    
    TRACE("****************************************");
    for(int i=0;i<SINLOG_TABLESIZE;i++) {
        pos += sprintf(buffer+pos, "%d ", sinLogTable[i]);
        if ( pos > 90 ) {
            TRACE("SINLOGTABLE: %s" ,buffer);
            buffer[0] = 0;
            pos = 0;
        }
    }
    TRACE("SINLOGTABLE: %s", buffer);
    buffer[0] = 0;
    pos = 0;
    TRACE("----------------------------------------");    
    for(int i=0;i<SINEXP_TABLESIZE;i++) {
        pos += sprintf(buffer+pos, "%d ", sinExpTable[i]);
        if ( pos > 90 ) {
            TRACE("SINEXTTABLE: %s" ,buffer);
            buffer[0] = 0;
            pos = 0;
        }
    }
    TRACE("SINEXTTABLE: %s", buffer);
    TRACE("****************************************");
#endif
}

#if defined(AVDEXED_TESTING)
const uint16_t* EngineMkI::testSinLogTable() { return sinLogTable; }
const uint16_t* EngineMkI::testSinExpTable() { return sinExpTable; }
#endif

inline int32_t mkiSin(int32_t phase, uint16_t env) {
    uint16_t expVal = sinLog(phase >> (22 - SINLOG_BITDEPTH)) + (env);
    //int16_t expValShow = expVal;
    
    const bool isSigned = expVal & NEGATIVE_BIT;
    expVal &= ~NEGATIVE_BIT;
    
    const uint16_t SINEXP_FILTER = 0x3FF;
    uint16_t result = 4096 + sinExpTable[( expVal & SINEXP_FILTER ) ^ SINEXP_FILTER];
    
    //uint16_t resultB4 = result;
    result >>= ( expVal >> 10 ); // exp
    
#ifdef MKIDEBUG
    if ( ( time(NULL) % 5 ) == 0 ) {
        if ( expValShow < 0 ) {
            expValShow = (expValShow + 0x7FFF) * -1;
        }
        //TRACE(",%d,%d,%d,%d,%d,%d", phase >> (22 - SINLOG_BITDEPTH), env, expValShow, ( expVal & SINEXP_FILTER ) ^ SINEXP_FILTER, resultB4, result);
    }
#endif
    
    // -result - 1 is ~result: the sign as a mask instead of a branch.
    return int32_t((uint32_t(result) ^ (0u - uint32_t(isSigned))) << 13);
}

void EngineMkI::compute(int32_t *output, const int32_t *input,
                        int32_t phase0, int32_t freq,
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

void EngineMkI::compute_pure(int32_t *output, int32_t phase0, int32_t freq,
                             int32_t gain1, int32_t gain2, bool add) {
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

void EngineMkI::compute_fb(int32_t *output, int32_t phase0, int32_t freq,
                           int32_t gain1, int32_t gain2,
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

// exclusively used for ALGO 6 with feedback
void EngineMkI::compute_fb2(int32_t *output, FmOpParams *parms, int32_t gain01, int32_t gain02, int32_t *fb_buf, int fb_shift) {
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
        
        // op 0
        gain[0] += dgain[0];
        y0 = y;
        y = mkiSin(phase[0]+scaled_fb, gain[0]);
        phase[0] += parms[0].freq;
        
        // op 1
        gain[1] += dgain[1];
        y = mkiSin(phase[1]+y, gain[1]);
        phase[1] += parms[1].freq;
        
        output[i] = y;
    }
    fb_buf[0] = y0;
    fb_buf[1] = y;
}

// exclusively used for ALGO 4 with feedback
void EngineMkI::compute_fb3(int32_t *output, FmOpParams *parms, int32_t gain01, int32_t gain02, int32_t *fb_buf, int fb_shift) {
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
        
        // op 0
        gain[0] += dgain[0];
        y0 = y;
        y = mkiSin(phase[0]+scaled_fb, gain[0]);
        phase[0] += parms[0].freq;
        
        // op 1
        gain[1] += dgain[1];
        y = mkiSin(phase[1]+y, gain[1]);
        phase[1] += parms[1].freq;
        
        // op 2
        gain[2] += dgain[2];
        y = mkiSin(phase[2]+y, gain[2]);
        phase[2] += parms[2].freq;
        
        output[i] = y;
    }
    fb_buf[0] = y0;
    fb_buf[1] = y;
}

void EngineMkI::render(int32_t *output, FmOpParams *params, int algorithm, int32_t *fb_buf, int32_t feedback_shift) {
    const int kLevelThresh = ENV_MAX-100;
    FmAlgorithm alg = algorithms[algorithm];
    bool has_contents[3] = { true, false, false };
    bool fb_on = feedback_shift < 16;

    switch(algorithm) {
        case 3 : case 5 :
            if ( fb_on )
                alg.ops[0] = 0xc4;
    }
    
    // When no operator that writes to the output sounds this block, the other
    // operators' kernels would only fill buses that nothing reads: they are not
    // computed. Every gain, flag and branch is still followed as before, so a
    // feedback operator still computes (its memory carries on to later blocks)
    // and nothing that is kept changes.
    // (The 2- and 3-operator chains of algorithms 4 and 6 write to the output.)
    int32_t gains1[6], gains2[6];
    bool outputSilent = true;
    for (int op = 0; op < 6; op++) {
        gains1[op] = params[op].gain_out == 0 ? (ENV_MAX-1) : params[op].gain_out;
        gains2[op] = ENV_MAX-(params[op].level_in >> (28-ENV_BITDEPTH));
        if ((alg.ops[op] & 3) == 0 && (gains1[op] <= kLevelThresh || gains2[op] <= kLevelThresh))
            outputSilent = false;
    }

    for (int op = 0; op < 6; op++) {
        int flags = alg.ops[op];
        bool add = (flags & OUT_BUS_ADD) != 0;
        FmOpParams &param = params[op];
        int inbus = (flags >> 4) & 3;
        int outbus = flags & 3;
        int32_t *outptr = (outbus == 0) ? output : buf_[outbus - 1].get();
        int32_t gain1 = gains1[op];
        int32_t gain2 = gains2[op];
        param.gain_out = gain2;
        
        if (gain1 <= kLevelThresh || gain2 <= kLevelThresh) {
            
            if (!has_contents[outbus]) {
                add = false;
            }
            
            if (inbus == 0 || !has_contents[inbus]) {
                // PG: this is my 'dirty' implementation of FB for 2 and 3 operators...
                if ((flags & 0xc0) == 0xc0 && fb_on) {
                    switch ( algorithm ) {
                        // three operator feedback, process exception for ALGO 4
                        case 3 :
                            compute_fb3(outptr, params, gain1, gain2, fb_buf, min((feedback_shift+2), 16));
                            params[1].phase += params[1].freq << LG_N; // hack, we already processed op-5 - op-4
                            params[2].phase += params[2].freq << LG_N; // yuk yuk
                            op += 2; // ignore the 2 other operators
                            break;
                        // two operator feedback, process exception for ALGO 6
                        case 5 :
                            compute_fb2(outptr, params, gain1, gain2, fb_buf, min((feedback_shift+2), 16));
                            params[1].phase += params[1].freq << LG_N;  // yuk, hack, we already processed op-5
                            op++; // ignore next operator;
                            break;
                        case 31 :
                            // one operator feedback, process exception for ALGO 32
                            compute_fb(outptr, param.phase, param.freq, gain1, gain2, fb_buf, min((feedback_shift+2), 16), add);
                            break;
                        default:
                            // one operator feedback, normal process
                            compute_fb(outptr, param.phase, param.freq, gain1, gain2, fb_buf, feedback_shift, add);
                            break;
                    }
                } else if (!outputSilent) {
                    compute_pure(outptr, param.phase, param.freq, gain1, gain2, add);
                }
            } else if (!outputSilent) {
                compute(outptr, buf_[inbus - 1].get(), param.phase, param.freq, gain1, gain2, add);
            }
            
            has_contents[outbus] = true;
        } else if (!add) {
            has_contents[outbus] = false;
        }
        param.phase += param.freq << LG_N;
    }
}

// ---- Several voices at a time (renderFour, renderEight) ----

namespace {
// One operator of L lanes: what render passes to its kernel for each.
template <int L>
struct Operators {
    int32_t phase[L] = {}, freq[L] = {}, gain1[L] = {}, gain2[L] = {};
    bool run[L] = {};       // this lane computes this operator with this kernel
    bool add[L] = {};       // adds to what its bus holds
    bool fromBus[L] = {};   // modulated (compute) rather than pure (compute_pure)
    bool any(int first = 0, int count = L) const {
        for (int l = first; l < first + count; l++)
            if (run[l]) return true;
        return false;
    }
};

template <int L>
using Rows = int32_t[N][L];

// compute / compute_pure for the lanes in `run`, each lane as alone (a pure
// one is a modulated one with no input: phase + 0); the other lanes keep
// their samples.
template <int L>
void modulatedScalar(Rows<L> &out, const Rows<L> *in, const Operators<L> &o) {
    int32_t dgain[L], gain[L], phase[L];
    for (int l = 0; l < L; l++) {
        dgain[l] = (o.gain2[l] - o.gain1[l] + (N >> 1)) >> LG_N;
        gain[l] = o.gain1[l];
        phase[l] = o.phase[l];
    }
    for (int i = 0; i < N; i++)
        for (int l = 0; l < L; l++) {
            if (!o.run[l]) continue;
            gain[l] += dgain[l];
            const int32_t input = o.fromBus[l] ? (*in)[i][l] : 0;
            const int32_t y = mkiSin(phase[l] + input, gain[l]);
            out[i][l] = y + (o.add[l] ? out[i][l] : 0);
            phase[l] += o.freq[l];
        }
}

#if defined(__ARM_NEON)
// Four 16-bit table entries, each loaded straight into the low half of its
// 32-bit lane (the high halves stay zero).
inline uint32x4_t gather(const uint16_t *table, uint32x4_t index) {
    uint16x8_t v = vdupq_n_u16(0);
    v = vld1q_lane_u16(table + vgetq_lane_u32(index, 0), v, 0);
    v = vld1q_lane_u16(table + vgetq_lane_u32(index, 1), v, 2);
    v = vld1q_lane_u16(table + vgetq_lane_u32(index, 2), v, 4);
    v = vld1q_lane_u16(table + vgetq_lane_u32(index, 3), v, 6);
    return vreinterpretq_u32_u16(v);
}

// modulatedScalar for lanes first .. first + 3 in one register: mkiSin step by
// step (the two table reads go lane by lane). phi = (uint16) (p >> 12): the
// bits 10 and 11 of its quarter are bits 22 and 23 of p, so the mirror and the
// sign come straight from p; (m & 0x3FF) ^ 0x3FF is ~m & 0x3FF.
template <int L>
void modulatedNeon(Rows<L> &out, const Rows<L> *in, const Operators<L> &o, int first) {
    const auto mask = [first](const bool (&flags)[L]) {
        const uint32_t m[4] = { flags[first] ? ~0u : 0u, flags[first + 1] ? ~0u : 0u,
                                flags[first + 2] ? ~0u : 0u, flags[first + 3] ? ~0u : 0u };
        return vld1q_u32(m);
    };
    const uint32x4_t run = mask(o.run), add = mask(o.add), fromBus = mask(o.fromBus);
    int32x4_t gain = vld1q_s32(o.gain1 + first);
    const int32x4_t dgain = vshrq_n_s32(vaddq_s32(vsubq_s32(vld1q_s32(o.gain2 + first), gain), vdupq_n_s32(N >> 1)), LG_N);
    int32x4_t phase = vld1q_s32(o.phase + first);
    const int32x4_t freq = vld1q_s32(o.freq + first);
    const uint32x4_t filter = vdupq_n_u32(SINLOG_TABLESIZE - 1);
    for (int i = 0; i < N; i++) {
        gain = vaddq_s32(gain, dgain);
        int32x4_t p = phase;
        if (in != nullptr)
            p = vaddq_s32(p, vreinterpretq_s32_u32(vandq_u32(vreinterpretq_u32_s32(vld1q_s32(&(*in)[i][first])), fromBus)));
        // sinLog: the quarter as masks.
        const int32x4_t mirror = vshrq_n_s32(vshlq_n_s32(p, 9), 31);
        const uint32x4_t index = vandq_u32(vreinterpretq_u32_s32(veorq_s32(vshrq_n_s32(p, 22 - SINLOG_BITDEPTH), mirror)), filter);
        const uint32x4_t sign = vandq_u32(vreinterpretq_u32_s32(vshrq_n_s32(vshlq_n_s32(p, 8), 31)), vdupq_n_u32(NEGATIVE_BIT));
        const uint32x4_t logValue = vorrq_u32(gather(sinLogTable, index), sign);
        // mkiSin: (uint16) log + envelope, sign apart, then the exp table.
        const uint32x4_t expVal = vandq_u32(vaddq_u32(logValue, vandq_u32(vreinterpretq_u32_s32(gain), vdupq_n_u32(0xFFFF))),
                                            vdupq_n_u32(0xFFFF));
        const uint32x4_t isSigned = vshrq_n_u32(expVal, 15);
        const uint32x4_t magnitude = vandq_u32(expVal, vdupq_n_u32(0x7FFF));
        const uint32x4_t expIndex = vbicq_u32(vdupq_n_u32(0x3FF), magnitude);
        const uint32x4_t exps = vaddq_u32(gather(sinExpTable, expIndex), vdupq_n_u32(4096));
        uint32x4_t result = vshlq_u32(exps, vnegq_s32(vreinterpretq_s32_u32(vshrq_n_u32(magnitude, 10))));
        result = veorq_u32(result, vreinterpretq_u32_s32(vnegq_s32(vreinterpretq_s32_u32(isSigned))));   // ~result when signed
        const int32x4_t y = vreinterpretq_s32_u32(vshlq_n_u32(result, 13));
        const int32x4_t old = vld1q_s32(&out[i][first]);
        const int32x4_t value = vaddq_s32(y, vreinterpretq_s32_u32(vandq_u32(vreinterpretq_u32_s32(old), add)));
        vst1q_s32(&out[i][first], vbslq_s32(run, value, old));
        phase = vaddq_s32(phase, freq);
    }
}
#endif

template <int L>
void modulated(Rows<L> &out, const Rows<L> *in, const Operators<L> &o) {
#if defined(__ARM_NEON)
  #if defined(AVDEXED_TESTING)
    if (!EngineMkI::useSimd) {
        modulatedScalar<L>(out, in, o);
        return;
    }
  #endif
    for (int first = 0; first < L; first += 4)
        if (o.any(first, 4)) modulatedNeon<L>(out, in, o, first);
#else
    modulatedScalar<L>(out, in, o);
#endif
}

// compute_fb for the lanes in `run`: L feedback chains side by side, each lane
// as alone (its own memory). Every chain is computed, so no lane is tested
// per sample and the chains overlap; a lane not in `run` keeps its samples
// and memory.
template <int L>
void feedback(Rows<L> &out, const Operators<L> &o, int32_t *const fb_bufs[L], int fb_shift) {
    int32_t dgain[L], gain[L], phase[L], y0[L], y[L], keep[L], take[L];
    for (int l = 0; l < L; l++) {
        dgain[l] = (o.gain2[l] - o.gain1[l] + (N >> 1)) >> LG_N;
        gain[l] = o.gain1[l];
        phase[l] = o.phase[l];
        y0[l] = o.run[l] ? fb_bufs[l][0] : 0;
        y[l] = o.run[l] ? fb_bufs[l][1] : 0;
        take[l] = o.run[l] ? -1 : 0;                         // the new sample
        keep[l] = o.run[l] ? (o.add[l] ? -1 : 0) : -1;      // what the bus held
    }
    for (int i = 0; i < N; i++) {
        int32_t next[L];
        for (int l = 0; l < L; l++) {
            gain[l] += dgain[l];
            const int32_t scaled_fb = (y0[l] + y[l]) >> (fb_shift + 1);
            next[l] = mkiSin(phase[l] + scaled_fb, gain[l]);
            phase[l] += o.freq[l];
        }
        for (int l = 0; l < L; l++) {
            y0[l] = y[l];
            y[l] = next[l];
            out[i][l] = (next[l] & take[l]) + (out[i][l] & keep[l]);
        }
    }
    for (int l = 0; l < L; l++)
        if (o.run[l]) {
            fb_bufs[l][0] = y0[l];
            fb_bufs[l][1] = y[l];
        }
}

// EngineMkI::render for `lanes` voices side by side (see renderFour).
template <int L>
int renderLanes(Rows<L> &output, FmOpParams *const params[], int32_t *const fb_bufs[], int lanes,
                const FmAlgorithm &alg, int algorithm, int32_t feedback_shift) {
    const int kLevelThresh = ENV_MAX-100;
    const bool fb_on = feedback_shift < 16;
    const int fbShift = algorithm == 31 ? min((feedback_shift+2), 16) : feedback_shift;

    // Each lane's gains and whether anything of it reaches the output (render's first pass).
    int32_t gains1[L][6], gains2[L][6];
    bool outputSilent[L], hasContents[L][3];
    for (int l = 0; l < lanes; l++) {
        outputSilent[l] = true;
        hasContents[l][0] = true;
        hasContents[l][1] = hasContents[l][2] = false;
        for (int op = 0; op < 6; op++) {
            const FmOpParams &param = params[l][op];
            gains1[l][op] = param.gain_out == 0 ? (ENV_MAX-1) : param.gain_out;
            gains2[l][op] = ENV_MAX-(param.level_in >> (28-ENV_BITDEPTH));
            if ((alg.ops[op] & 3) == 0 && (gains1[l][op] <= kLevelThresh || gains2[l][op] <= kLevelThresh))
                outputSilent[l] = false;
        }
    }

    // The output starts at zero. A bus is read only by a lane that wrote it
    // earlier in this block (render's "has contents"), or by a lane whose
    // result is not kept: what it held before does not matter.
    for (auto &sample : output)
        for (auto &lane : sample) lane = 0;
    alignas(16) Rows<L> buses[2];
    int32_t idleMemory[2] = { 0, 0 };
    int32_t *feedbackMemory[L];
    for (int l = 0; l < L; l++) feedbackMemory[l] = l < lanes ? fb_bufs[l] : idleMemory;
    int wrote = 0;

    for (int op = 0; op < 6; op++) {
        const int flags = alg.ops[op];
        const bool addFlag = (flags & OUT_BUS_ADD) != 0;
        const int inbus = (flags >> 4) & 3;
        const int outbus = flags & 3;
        const bool fb = (flags & 0xc0) == 0xc0 && fb_on;
        // The lanes' data once; which kernel each lane runs (if any) apart.
        Operators<L> mod;
        bool fedRun[L] = {}, fedAdd[L] = {};
        for (int l = 0; l < lanes; l++) {
            FmOpParams &param = params[l][op];
            const int32_t gain1 = gains1[l][op];
            const int32_t gain2 = gains2[l][op];
            param.gain_out = gain2;
            mod.phase[l] = param.phase;
            mod.freq[l] = param.freq;
            mod.gain1[l] = gain1;
            mod.gain2[l] = gain2;
            if (gain1 <= kLevelThresh || gain2 <= kLevelThresh) {
                const bool add = addFlag && hasContents[l][outbus];
                const bool fromBus = ! (inbus == 0 || !hasContents[l][inbus]);
                if (!fromBus && fb) {
                    fedRun[l] = true;
                    fedAdd[l] = add;
                } else if (!outputSilent[l]) {
                    mod.run[l] = true;
                    mod.add[l] = add;
                    mod.fromBus[l] = fromBus;
                }
                hasContents[l][outbus] = true;
            } else if (!addFlag) {
                hasContents[l][outbus] = false;
            }
            param.phase += param.freq << LG_N;
        }
        Rows<L> &out = outbus == 0 ? output : buses[outbus - 1];
        const Rows<L> *in = inbus == 0 ? nullptr : &buses[inbus - 1];
        bool anyFed = false;
        for (int l = 0; l < lanes; l++) {
            if (outbus == 0 && (mod.run[l] || fedRun[l])) wrote |= 1 << l;
            anyFed |= fedRun[l];
        }
        if (mod.any())
            modulated<L>(out, in, mod);
        if (anyFed) {
            Operators<L> fed = mod;
            for (int l = 0; l < L; l++) {
                fed.run[l] = fedRun[l];
                fed.add[l] = fedAdd[l];
            }
            feedback<L>(out, fed, feedbackMemory, fbShift);
        }
    }
    return wrote;
}
}

EngineMkI::Work EngineMkI::workFor(const FmOpParams *params, int algorithm) {
    // render's thresholds; for algorithms 4 and 6 with feedback (never
    // batched) operator 6 writes to the output (0xc4), which only makes a
    // sounding voice more likely to read as one.
    const int kLevelThresh = ENV_MAX-100;
    const FmAlgorithm &alg = algorithms[algorithm];
    bool any = false;
    for (int op = 0; op < 6; op++) {
        const int32_t gain1 = params[op].gain_out == 0 ? (ENV_MAX-1) : params[op].gain_out;
        const int32_t gain2 = ENV_MAX-(params[op].level_in >> (28-ENV_BITDEPTH));
        if (gain1 <= kLevelThresh || gain2 <= kLevelThresh) {
            if ((alg.ops[op] & 3) == 0 || ((algorithm == 3 || algorithm == 5) && op == 0))
                return Work::sound;
            any = true;
        }
    }
    return any ? Work::feedbackOnly : Work::nothing;
}

int EngineMkI::renderFour(FourVoices &output, FmOpParams *const params[4], int32_t *const fb_bufs[4], int lanes,
                          int algorithm, int32_t feedback_shift) {
    // As render, lane by lane: algorithms 4 and 6 with feedback never come here.
    return renderLanes<4>(output, params, fb_bufs, lanes, algorithms[algorithm], algorithm, feedback_shift);
}

int EngineMkI::renderEight(EightVoices &output, FmOpParams *const params[8], int32_t *const fb_bufs[8], int lanes,
                           int algorithm, int32_t feedback_shift) {
    return renderLanes<8>(output, params, fb_bufs, lanes, algorithms[algorithm], algorithm, feedback_shift);
}
