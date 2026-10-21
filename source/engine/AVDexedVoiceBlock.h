// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../msfa/synth.h"
#include <cstdint>

class Dx7Note;
class FmCore;

/** One engine block of the given voices, already prepared (Dx7Note::prepare):
    each voice is clipped as the instrument always clipped a voice and added to
    `sum` as whole 1/32768ths. Voices on Mark I that share an algorithm, a
    feedback amount and the kind of work this block go up to eight at a time
    (EngineMkI::renderEight), the others one by one (Dx7Note::render): the
    same samples either way. Used by the simple path and by every layer, which
    therefore stay alike. */
void AVRenderVoiceBlock(Dx7Note *const *notes, int count, FmCore *core, int32_t *sum);
