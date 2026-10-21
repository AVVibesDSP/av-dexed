/*
 * Copyright 2014 Pascal Gauthier.
 * Copyright 2012 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ENGINEMKI_H_INCLUDED
#define ENGINEMKI_H_INCLUDED

#include "msfa/synth.h"
#include "msfa/aligned_buf.h"
#include "msfa/fm_op_kernel.h"
#include "msfa/controllers.h"
#include "msfa/fm_core.h"


class EngineMkI : public FmCore {
public:
    EngineMkI();
    
    void render(int32_t *output, FmOpParams *params, int algorithm, int32_t *fb_buf, int32_t feedback_shift) override;
    
    void compute(int32_t *output, const int32_t *input, int32_t phase0, int32_t freq, int32_t gain1, int32_t gain2,
                    bool add);
    
	void compute_pure(int32_t *output, int32_t phase0, int32_t freq, int32_t gain1, int32_t gain2,
                         bool add);
    
    void compute_fb(int32_t *output, int32_t phase0, int32_t freq, int32_t gain1, int32_t gain2,
                       int32_t *fb_buf, int fb_gain, bool add);
    
    void compute_fb2(int32_t *output, FmOpParams *params, int32_t gain01, int32_t gain02, int32_t *fb_buf, int fb_shift);
    
    void compute_fb3(int32_t *output, FmOpParams *params, int32_t gain01, int32_t gain02, int32_t *fb_buf, int fb_shift);

    /** Four or eight voices' samples side by side: [sample][voice]. */
    using FourVoices = int32_t[N][4];
    using EightVoices = int32_t[N][8];
    /** render for up to four voices (`lanes`) that share `algorithm` and
        `feedback_shift`: each lane exactly as render computes it alone into
        a buffer of zeros (its own gains, buses, branches and feedback
        memory), the operator kernels four voices at a time (NEON on arm64;
        elsewhere four voices interleaved in plain C++). `output` is written
        in full; returns which lanes wrote anything (bit l for lane l). */
    int renderFour(FourVoices &output, FmOpParams *const params[4], int32_t *const fb_bufs[4], int lanes,
                   int algorithm, int32_t feedback_shift);
    /** renderFour for up to eight voices: the operator kernels as two NEON
        halves, the feedback chains eight side by side (a feedback operator is a
        chain of samples; more chains at once keep the processor busier). */
    int renderEight(EightVoices &output, FmOpParams *const params[8], int32_t *const fb_bufs[8], int lanes,
                    int algorithm, int32_t feedback_shift);
    /** What render will compute for a voice this block, by its thresholds:
        nothing (it only keeps its gains and phases; it writes nothing), only
        its feedback operator (nothing reaches the output), or its sound.
        Voices are batched with ones that do the same work. */
    enum class Work { nothing, feedbackOnly, sound };
    static Work workFor(const FmOpParams *params, int algorithm);
    /** Algorithms 4 and 6 with feedback run their 2- and 3-operator chains
        one voice at a time (render). */
    static bool canRenderFour(int algorithm, int32_t feedback_shift) {
        return ! ((algorithm == 3 || algorithm == 5) && feedback_shift < 16);
    }

#if defined(AVDEXED_TESTING)
    /** Off: renderFour's and renderEight's lanes go through plain C++ on arm64 too (as on x86). */
    static inline bool useSimd = true;
    /** The sine tables, for the kernel tests' copy of the original code. */
    static const uint16_t* testSinLogTable();
    static const uint16_t* testSinExpTable();
#endif
};


#endif  // ENGINEMKI_H_INCLUDED
