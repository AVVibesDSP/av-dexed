// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <functional>

/** A drawing kept as an image made at the screen's own pixels: the window's
    scale and the drawing's sub-pixel position are part of it, so drawing it
    back is a plain copy with no resampling, and gives the pixels the drawing
    itself gives (up to one level of rounding where shapes overlap). JUCE's
    setBufferedToImage resamples whenever the zoom puts a component between
    pixels, which softens text and edges.

    Without a window (a snapshot), or with a transform other than a scale and
    an offset, the drawing is painted directly. */
class AVDexedPixelCache {
public:
    /** Draws what `paintContent` paints in `owner`'s coordinates, all of
        which lies inside `area`. `g` must be in `owner`'s coordinates. The
        image is made again when it is missing, after invalidate(), or when
        the scale, the sub-pixel position or the area changed. */
    void draw(juce::Graphics& g, const juce::Component& owner, juce::Rectangle<float> area,
              const std::function<void(juce::Graphics&)>& paintContent) {
        using namespace juce;
        if (area.isEmpty() || !g.clipRegionIntersects(area.getSmallestIntegerContainer())) return;
        const auto placement = placementOf(g, owner);
        if (!placement.valid || !enabled) {
            Graphics::ScopedSaveState state(g);
            paintContent(g);
            return;
        }
        // The area on the screen's pixels, widened to whole pixels.
        const auto pixels = (area * placement.pixelsPerUnit + placement.origin).getSmallestIntegerContainer();
        const auto offset = placement.origin - pixels.getPosition().toFloat();
        if (image.isNull() || pixels.getWidth() != image.getWidth() || pixels.getHeight() != image.getHeight()
            || placement.pixelsPerUnit != madeScale || offset != madeOffset) {
            image = Image(Image::ARGB, jmax(1, pixels.getWidth()), jmax(1, pixels.getHeight()), true);
            Graphics imageGraphics(image);
            imageGraphics.addTransform(AffineTransform::scale(placement.pixelsPerUnit).translated(offset));
            imageGraphics.reduceClipRegion(area.getSmallestIntegerContainer());
            paintContent(imageGraphics);
            madeScale = placement.pixelsPerUnit;
            madeOffset = offset;
        }
        // Each image pixel lands on one screen pixel (give or take float
        // rounding): nearest-pixel sampling copies it as it is.
        Graphics::ScopedSaveState state(g);
        g.setImageResamplingQuality(Graphics::lowResamplingQuality);
        g.drawImageTransformed(image, AffineTransform::translation(-offset).scaled(1.0f / placement.pixelsPerUnit), false);
    }

    void invalidate() { image = {}; }

    /** Off: everything is painted directly (the tools compare both ways on screen). */
    static inline bool enabled = true;

private:
    struct Placement {
        bool valid = false;
        float pixelsPerUnit = 1.0f;
        juce::Point<float> origin;   // owner's (0, 0) on the window's pixels
    };

    static Placement placementOf(juce::Graphics& g, const juce::Component& owner) {
        using namespace juce;
        auto* peer = owner.getPeer();
        if (peer == nullptr) return {};
        auto& top = peer->getComponent();
        const auto origin = top.getLocalPoint(&owner, juce::Point<float>());
        const auto right = top.getLocalPoint(&owner, juce::Point<float>(1.0f, 0.0f)) - origin;
        const auto down = top.getLocalPoint(&owner, juce::Point<float>(0.0f, 1.0f)) - origin;
        const float scale = right.x;
        if (scale <= 0.0f || std::abs(right.y) > 1.0e-4f || std::abs(down.x) > 1.0e-4f
            || std::abs(down.y - scale) > 1.0e-4f * scale)
            return {};
        // The window's pixels per point: 1 or 2 (Retina) on a Mac. Any other
        // value means another target (a snapshot at its own scale).
        const float physical = g.getInternalContext().getPhysicalPixelScaleFactor();
        const float backing = physical / scale;
        const float wholeBacking = std::round(backing);
        if (wholeBacking < 1.0f || std::abs(backing - wholeBacking) > 0.01f) return {};
        return { true, scale * wholeBacking, origin * wholeBacking };
    }

    juce::Image image;
    float madeScale = 0.0f;
    juce::Point<float> madeOffset;
};
