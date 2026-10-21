// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "AVDexedAnimationClock.h"
#include "AVDexedPixelCache.h"
#include "AVDexedNativeFade.h"
#include <functional>

/** A picture of part of a component, made at its window's own pixels (as
    AVDexedPixelCache): drawn back, every pixel lands where it was painted, so
    it shows exactly what the component shows there, with nothing resampled. */
class AVDexedScreenImage {
public:
    /** Paints `paintContent` (given a Graphics in `owner`'s coordinates) into a
        picture of `area`, reusing the memory of the last one when it is as big.
        An opaque picture is not cleared first, so what it paints must cover the
        area. False (and nothing made) when the owner is not in a window, or is
        rotated or stretched there. */
    bool make(const juce::Component& owner, juce::Rectangle<float> area, bool opaque,
              const std::function<void(juce::Graphics&)>& paintContent) {
        using namespace juce;
        const auto placement = AVDexedPixelCache::placementOf(owner, AVDexedPixelCache::screenBacking(owner));
        if (! placement.valid || area.isEmpty()) {
            reset();
            return false;
        }
        const auto pixels = (area * placement.pixelsPerUnit + placement.origin).getSmallestIntegerContainer();
        // Always 32-bit: CoreGraphics blends those with its vector code; a
        // 24-bit RGB picture (JUCE's opaque format on a Mac) took a scalar path
        // several times slower.
        const auto format = Image::ARGB;
        const int width = jmax(1, pixels.getWidth()), height = jmax(1, pixels.getHeight());
        if (image.isNull() || image.getWidth() != width || image.getHeight() != height || image.getReferenceCount() > 1) {
            image = Image(format, width, height, ! opaque);
            // Only ever drawn, and made again when needed: on Windows it then
            // stays on the GPU instead of being copied back to memory after painting.
            image.setBackupEnabled(false);
        } else if (! opaque) {
            image.clear(image.getBounds());
        }
        madeArea = area;
        pixelArea = { pixels.getX(), pixels.getY(), width, height };
        offset = placement.origin - pixels.getPosition().toFloat();
        pixelsPerUnit = placement.pixelsPerUnit;
        {
            Graphics g(image);
            g.addTransform(AffineTransform::scale(pixelsPerUnit).translated(offset));
            g.reduceClipRegion(area.getSmallestIntegerContainer());
            paintContent(g);
        }
        return true;
    }

    /** True when it holds a picture of `area` that lands on `owner`'s window
        pixels as they are now (same place, same scale). */
    bool matches(const juce::Component& owner, juce::Rectangle<float> area) const {
        if (image.isNull() || area != madeArea) return false;
        const auto placement = AVDexedPixelCache::placementOf(owner, AVDexedPixelCache::screenBacking(owner));
        if (! placement.valid || placement.pixelsPerUnit != pixelsPerUnit) return false;
        const auto pixels = (area * placement.pixelsPerUnit + placement.origin).getSmallestIntegerContainer();
        return placement.origin - pixels.getPosition().toFloat() == offset;
    }

    /** Draws the picture at `alpha`; `g` must be in the owner's coordinates. */
    void draw(juce::Graphics& g, float alpha) const {
        using namespace juce;
        if (image.isNull() || alpha <= 0.0f) return;
        Graphics::ScopedSaveState state(g);
        g.setImageResamplingQuality(Graphics::lowResamplingQuality);
        g.setOpacity(jmin(1.0f, alpha));
        g.drawImageTransformed(image, AffineTransform::translation(-offset).scaled(1.0f / pixelsPerUnit), false);
    }

    bool isValid() const noexcept { return image.isValid(); }
    void reset() { image = {}; }

    /** The picture, for the window's compositor, at `pixelArea` (the window's pixels). */
    AVDexedNativeFade::Picture toNative(float fromAlpha, float toAlpha) const { return { image, pixelArea, fromAlpha, toAlpha }; }

private:
    juce::Image image;
    juce::Rectangle<int> pixelArea;
    juce::Rectangle<float> madeArea;
    float pixelsPerUnit = 1.0f;
    juce::Point<float> offset;
};

/** Dims a component (setAlpha) as JUCE does, in a transparency layer, but
    with the layer the size of the component. JUCE opens a see-through
    child's layer over everything being repainted around it (it does not clip
    to the child first): on the Layers page with the layers off, a full
    repaint composited 64 layers as big as the page (~130 ms). The pixels are
    the same: outside the component the layer held nothing.

    Use AVDexedDim::setAlpha() instead of Component::setAlpha(): it installs
    this (as the component's CachedComponentImage, through which JUCE then
    paints it) while the alpha is below 1, and removes it at 1. */
class AVDexedDim final : public juce::CachedComponentImage {
public:
    static void setAlpha(juce::Component& component, float alpha) {
        component.setAlpha(alpha);
        const bool dimmed = component.getAlpha() < 1.0f;
        if (dimmed && dynamic_cast<AVDexedDim*>(component.getCachedComponentImage()) == nullptr)
            component.setCachedComponentImage(new AVDexedDim(component));
        else if (! dimmed && dynamic_cast<AVDexedDim*>(component.getCachedComponentImage()) != nullptr)
            component.setCachedComponentImage(nullptr);
    }

    void paint(juce::Graphics& g) override {
        juce::Graphics::ScopedSaveState state(g);
        // A little beyond the bounds: at a zoom that puts an edge between
        // pixels, a clip right on it would cut that pixel's antialiasing.
        if (! owner.isPaintingUnclipped()) g.reduceClipRegion(owner.getLocalBounds().expanded(2));
        if (g.isClipEmpty()) return;
        const float alpha = owner.getAlpha();
        if (alpha <= 0.0f) return;
        if (alpha < 1.0f) g.beginTransparencyLayer(alpha);
        owner.paintEntireComponent(g, true);
        if (alpha < 1.0f) g.endTransparencyLayer();
    }
    bool invalidateAll() override { return true; }
    bool invalidate(const juce::Rectangle<int>&) override { return true; }
    void releaseResources() override {}

private:
    explicit AVDexedDim(juce::Component& component) : owner(component) {}
    juce::Component& owner;
};

/** A cross-fade between two pictures over a third (what lies under both),
    drawn on the editor's animation clock (the screen's refresh) instead of
    ComponentAnimator's 50 Hz timer. Each frame draws three pictures and
    nothing else: what the pictures show is not painted while it runs (this
    component is opaque and covers it), and no transparency layer is needed.

    It reproduces ComponentAnimator::fadeOut() of one component (a picture of
    it fading from 1 to 0) together with fadeIn() of another (from 0 to 1),
    both linear over the same time, in their order on screen. */
class AVDexedCrossfade final : public juce::Component, private juce::Timer {
public:
    AVDexedCrossfade() {
        setOpaque(true);
        setInterceptsMouseClicks(false, false);
        setVisible(false);
    }

    /** The pictures, made in place by the owner before start() (they keep their
        memory from one fade to the next, and are let go a few seconds after). */
    AVDexedScreenImage under, outgoing, incoming;

    /** Makes `outgoing` what is shown now, when a fade runs (to fade from it to
        something else). False when none runs. */
    bool takeCurrentFrameAsOutgoing(juce::Rectangle<float> area) {
        if (! isRunning() || owner == nullptr) return false;
        if (! spare.make(*owner, area, true, [this](juce::Graphics& g) { paintFrame(g); })) return false;
        std::swap(spare, outgoing);
        return true;
    }

    /** Pictures are in `pictureOwner`'s coordinates; this component must cover
        what they show. `outgoingOnTop`: the component fading out is above the
        one fading in. */
    void start(const juce::Component& pictureOwner, bool outgoingOnTop, double durationMs, std::function<void()> finished) {
        stopTimer();
        owner = &pictureOwner;
        fromOnTop = outgoingOnTop;
        duration = juce::jmax(1.0, durationMs);
        onFinished = std::move(finished);
        startMs = juce::Time::getMillisecondCounterHiRes();
        progress = 0.0f;
        endingFrames = 0;
        // A tool's still frame: the fade held at testFreeze until finish().
        const float held = testFreeze;
        if (held >= 0.0f) {
            progress = juce::jlimit(0.0f, 1.0f, held);
            duration = 1.0e9;
        }
        // On a Mac the compositor fades the pictures (no painting per frame);
        // this component then only hides what is under them from JUCE's paints.
        std::vector<AVDexedNativeFade::Picture> pictures { under.toNative(1.0f, 1.0f) };
        auto lower = held >= 0.0f ? outgoing.toNative(1.0f - progress, 1.0f - progress) : outgoing.toNative(1.0f, 0.0f);
        auto upper = held >= 0.0f ? incoming.toNative(progress, progress) : incoming.toNative(0.0f, 1.0f);
        if (fromOnTop) std::swap(lower, upper);
        pictures.push_back(lower);
        pictures.push_back(upper);
        auto* peer = pictureOwner.getPeer();
        native = peer != nullptr && compositor.start(*peer, AVDexedPixelCache::screenBacking(pictureOwner), pictures, duration);
        setVisible(true);
        repaint();
        animation.start();
    }

    bool isRunning() const noexcept { return isVisible() || compositor.isRunning(); }

    /** For the tools: at 0…1, fades start and stay at that point. */
    static inline float testFreeze = -1.0f;

    /** For the tools: the frame shown now, painted by JUCE, and where it goes
        (the window's pixels). */
    juce::Image paintedFrame(juce::Rectangle<float> area, juce::Rectangle<int>& pixels) const {
        AVDexedScreenImage frame;
        if (owner == nullptr || ! frame.make(*owner, area, true, [this](juce::Graphics& g) { paintFrame(g); })) return {};
        const auto native = frame.toNative(1.0f, 1.0f);
        pixels = native.pixels;
        return native.image;
    }

    /** Ends at once, as if its time had run out. */
    void finish() {
        compositor.stop();
        if (! isVisible() && ! animation.isRunning()) return;
        animation.stop();
        setVisible(false);
        startTimer(3000);
        if (auto done = std::move(onFinished)) done();
    }

    void paint(juce::Graphics& g) override {
        if (owner == nullptr || native) return;   // the compositor shows the pictures
        g.addTransform(ownerToThis());   // the pictures are in the owner's coordinates
        paintFrame(g);
    }

private:
    void paintFrame(juce::Graphics& g) const {
        under.draw(g, 1.0f);
        const float in = progress, out = 1.0f - progress;
        if (fromOnTop) {
            incoming.draw(g, in);
            outgoing.draw(g, out);
        } else {
            outgoing.draw(g, out);
            incoming.draw(g, in);
        }
    }

    /** Maps the owner's coordinates to this component's. */
    juce::AffineTransform ownerToThis() const {
        const auto origin = getLocalPoint(owner, juce::Point<float>());
        const auto right = getLocalPoint(owner, juce::Point<float>(1.0f, 0.0f)) - origin;
        const auto down = getLocalPoint(owner, juce::Point<float>(0.0f, 1.0f)) - origin;
        return juce::AffineTransform(right.x, down.x, origin.x, right.y, down.y, origin.y);
    }

    bool tick(double now) {
        if (testFreeze >= 0.0f) return true;
        progress = juce::jlimit(0.0f, 1.0f, float((now - startMs) / duration));
        if (progress < 1.0f) {
            if (! native) repaint();
            return true;
        }
        if (! native) {
            finish();
            return false;
        }
        // The compositor still shows the last picture, which is what the
        // window shows under it: uncovered now, the page paints itself first,
        // and the pictures go a few frames later.
        if (endingFrames++ == 0) {
            setVisible(false);
            return true;
        }
        if (endingFrames < 3) return true;
        finish();
        return false;
    }

    void timerCallback() override {
        stopTimer();
        if (isRunning()) return;
        under.reset();
        outgoing.reset();
        incoming.reset();
        spare.reset();
    }

    const juce::Component* owner = nullptr;
    AVDexedScreenImage spare;
    AVDexedNativeFade compositor;
    bool native = false;
    int endingFrames = 0;
    bool fromOnTop = false;
    double startMs = 0.0, duration = 1.0;
    float progress = 0.0f;
    std::function<void()> onFinished;
    AVDexedAnimation animation { *this, [this](double now) { return tick(now); } };
};

/** ComponentAnimator::fadeIn(), fadeOut() and cancelAnimation() for a
    component over the editor (a dialog, Settings), on the editor's clock and
    from a picture of the component: at 50 Hz, ComponentAnimator painted the
    whole dialog (its blurred backdrop, panel and controls) into a
    transparency layer every frame, 10-19 ms a frame on an M4.

    The picture (made on the first frame, so the component can still be set up
    after fadeIn()) is shown by a cover above the component: on a Mac by the
    window's compositor, over the live window (no painting at all per frame);
    elsewhere painted with its alpha. The component itself is not painted while
    it fades (alpha 0 in, hidden out), as with ComponentAnimator's proxy. */
class AVDexedComponentFade final : public juce::Component, private juce::ComponentListener {
public:
    /** As ComponentAnimator::fadeIn(): unless already shown at alpha 1, shown
        from alpha 0 to 1. */
    static void fadeIn(juce::Component& target, double durationMs) {
        if (target.isVisible() && target.getAlpha() >= 1.0f) return;
        cancel(target);
        target.setAlpha(0.0f);
        target.setVisible(true);
        start(target, true, durationMs);
    }

    /** As ComponentAnimator::fadeOut(): hidden at once, a picture of it fading
        from its alpha to 0 (when it is on screen). */
    static void fadeOut(juce::Component& target, double durationMs) {
        cancel(target);
        if (target.isShowing() && durationMs > 0.0 && ! start(target, false, durationMs)) {
            // No picture at the window's pixels (a window at an unusual scale):
            // JUCE's own fade, which hides the component itself.
            juce::Desktop::getInstance().getAnimator().fadeOut(&target, juce::roundToInt(durationMs));
            return;
        }
        target.setVisible(false);
    }

    /** As ComponentAnimator::cancelAnimation(target, false): a fade-in stops
        where it is (the component then shows at that alpha); a fade-out's
        picture goes. */
    static void cancel(juce::Component& target) {
        if (auto* fade = find(target)) fade->end(false);
        juce::Desktop::getInstance().getAnimator().cancelAnimation(&target, false);
    }

    /** For the tools: at 0…1, fades hold at that point (once their picture
        is made) until cancel(). */
    static inline float testFreeze = -1.0f;

    /** True while a fade of the component (or of its picture) is on screen. */
    static bool isFading(juce::Component& target) {
        return find(target) != nullptr || juce::Desktop::getInstance().getAnimator().isAnimating(&target);
    }

    /** A component that goes away while it fades in: the fade forgets it. */
    static void forget(juce::Component& target) {
        if (auto* fade = find(target)) {
            fade->target = nullptr;
            fade->end(false);
        }
        juce::Desktop::getInstance().getAnimator().cancelAnimation(&target, false);
    }

    ~AVDexedComponentFade() override {
        compositor.stop();
        if (auto* p = parent.getComponent()) p->removeComponentListener(this);
    }

private:
    AVDexedComponentFade(juce::Component& component, bool in, double durationMs)
        : target(&component), parent(component.getParentComponent()), fadingIn(in), duration(juce::jmax(1.0, durationMs)) {
        setInterceptsMouseClicks(false, false);
        setWantsKeyboardFocus(false);
        setAccessible(false);
    }

    static AVDexedComponentFade* find(juce::Component& target) {
        if (auto* p = target.getParentComponent())
            for (auto* child : p->getChildren())
                if (auto* fade = dynamic_cast<AVDexedComponentFade*>(child); fade != nullptr && fade->target == &target)
                    return fade;
        return nullptr;
    }

    /** False when a fade-out's picture could not be made (nothing started). */
    static bool start(juce::Component& target, bool in, double durationMs) {
        auto* p = target.getParentComponent();
        if (p == nullptr) return false;
        auto* fade = new AVDexedComponentFade(target, in, durationMs);
        fade->setBounds(target.getBounds());
        // Just above the component: what was above it stays above its picture.
        p->addChildComponent(*fade, p->getIndexOfChildComponent(&target) + 1);
        p->addComponentListener(fade);
        fade->fromAlpha = in ? 0.0f : target.getAlpha();
        fade->toAlpha = in ? 1.0f : 0.0f;
        if (! in) {
            fade->takePicture();   // it is hidden right after this
            if (! fade->pictured) {
                fade->failed = true;
                fade->end(true);
                return false;
            }
        }
        fade->startMs = juce::Time::getMillisecondCounterHiRes();
        fade->animation.start();
        return true;
    }

    void takePicture() {
        if (target == nullptr || parent == nullptr) return;
        if (testFreeze >= 0.0f) progress = juce::jlimit(0.0f, 1.0f, testFreeze);
        auto& component = *target;
        pictured = picture.make(*parent, component.getBounds().toFloat(), false, [&component](juce::Graphics& g) {
            juce::Graphics::ScopedSaveState state(g);
            g.addTransform(juce::AffineTransform::translation(component.getPosition().toFloat()));
            component.paintEntireComponent(g, true);
        });
        if (! pictured) return;
        auto* peer = parent->getPeer();
        const float held = alphaAt(progress);
        native = peer != nullptr && compositor.start(*peer, AVDexedPixelCache::screenBacking(*parent),
                                                     { picture.toNative(held, testFreeze >= 0.0f ? held : toAlpha) },
                                                     duration * double(1.0f - progress));
        setVisible(! native);
        repaint();
    }

    float alphaAt(float t) const noexcept { return fromAlpha + (toAlpha - fromAlpha) * t; }

    void paint(juce::Graphics& g) override {
        if (! pictured || native || parent == nullptr) return;
        g.addTransform(juce::AffineTransform::translation(-getPosition().toFloat()));   // the picture is in the parent's coordinates
        picture.draw(g, alphaAt(progress));
    }

    bool tick(double now) {
        if (testFreeze < 0.0f) progress = juce::jlimit(0.0f, 1.0f, float((now - startMs) / duration));
        if (! pictured && ! failed) {
            takePicture();
            if (! pictured) {
                // No picture at the window's pixels: JUCE's own fade for the
                // time left (nothing is seen when it is not on screen at all).
                failed = true;
                auto* component = target.getComponent();
                const double left = duration * double(1.0f - progress);
                end(false);
                if (component != nullptr && component->isShowing())
                    juce::Desktop::getInstance().getAnimator().fadeIn(component, juce::roundToInt(left));
                else if (component != nullptr)
                    component->setAlpha(1.0f);
                return false;
            }
        }
        if (progress < 1.0f || testFreeze >= 0.0f) {
            if (! native) repaint();
            return true;
        }
        if (endingFrames++ == 0) {
            // The component shows itself now; the compositor's picture, which
            // looks the same, goes a few frames later.
            if (fadingIn && target != nullptr) target->setAlpha(1.0f);
            if (! native) {
                end(true);
                return false;
            }
            return true;
        }
        if (endingFrames < 3) return true;
        end(true);
        return false;
    }

    /** Ends the fade: complete (the component at its final alpha) or cut
        short (a fade-in then leaves the component at the alpha shown now). */
    void end(bool complete) {
        animation.stop();
        compositor.stop();
        if (target != nullptr && fadingIn) target->setAlpha(complete ? 1.0f : alphaAt(progress));
        target = nullptr;
        setVisible(false);
        if (auto* p = parent.getComponent()) {
            p->removeComponentListener(this);
            p->removeChildComponent(this);
        }
        deleteSoon();
    }

    void componentBeingDeleted(juce::Component&) override {
        animation.stop();
        compositor.stop();
        target = nullptr;
        parent = nullptr;
        deleteSoon();
    }

    /** Not from inside its own frame callback: after it (or at once by
        deleteAllIn(), whichever comes first). */
    void deleteSoon() {
        juce::MessageManager::callAsync([self = juce::Component::SafePointer<juce::Component>(this)] { delete self.getComponent(); });
    }

public:
    /** Deletes at once every fade over `parent`'s children (an editor that
        closes: nothing may wait for a later message). */
    static void deleteAllIn(juce::Component& parent) {
        for (int i = parent.getNumChildComponents(); --i >= 0;)
            if (auto* fade = dynamic_cast<AVDexedComponentFade*>(parent.getChildComponent(i))) {
                parent.removeComponentListener(fade);
                fade->animation.stop();
                fade->compositor.stop();
                delete fade;
            }
    }
private:

    juce::Component::SafePointer<juce::Component> target, parent;
    AVDexedScreenImage picture;
    AVDexedNativeFade compositor;
    bool fadingIn, pictured = false, native = false, failed = false;
    float fromAlpha = 0.0f, toAlpha = 1.0f, progress = 0.0f;
    double duration, startMs = 0.0;
    int endingFrames = 0;
    AVDexedAnimation animation { *this, [this](double now) { return tick(now); } };
};
