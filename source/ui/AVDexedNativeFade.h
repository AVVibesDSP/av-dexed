// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

/** Pictures faded by the window's compositor instead of by painting: on a Mac
    each picture becomes a Core Animation layer over the editor's view, and its
    opacity is animated by the GPU (CoreGraphics, which paints JUCE windows on
    the CPU, blends a picture with an alpha several nanoseconds a pixel: ~8 ms
    a frame for the page cross-fade at 1x, four times that on a Retina screen).
    Elsewhere start() returns false and the caller paints the fade itself
    (Direct2D on Windows already blends on the GPU). */
class AVDexedNativeFade {
public:
    struct Picture {
        juce::Image image;
        juce::Rectangle<int> pixels;   // where it goes, in the window's pixels
        float fromAlpha = 1.0f, toAlpha = 1.0f;
    };

    AVDexedNativeFade() = default;
    ~AVDexedNativeFade() { stop(); }
    AVDexedNativeFade(const AVDexedNativeFade&) = delete;
    AVDexedNativeFade& operator=(const AVDexedNativeFade&) = delete;

    /** Shows `pictures` (the first at the bottom) over `peer`'s view, each
        fading linearly from its fromAlpha to its toAlpha in `durationMs`, and
        holding there until stop(). `backing` is the window's pixels per point.
        False (and nothing shown) where this is not available. */
    bool start(juce::ComponentPeer& peer, float backing, const std::vector<Picture>& pictures, double durationMs);

    /** Removes the pictures at once. */
    void stop();

    bool isRunning() const noexcept { return container != nullptr; }

    /** Off: start() always returns false (the tools compare both ways). */
    static inline bool enabled = true;

private:
    void* container = nullptr;   // the CALayer holding the pictures (retained)
};

#if ! JUCE_MAC
inline bool AVDexedNativeFade::start(juce::ComponentPeer&, float, const std::vector<Picture>&, double) { return false; }
inline void AVDexedNativeFade::stop() {}
#endif
