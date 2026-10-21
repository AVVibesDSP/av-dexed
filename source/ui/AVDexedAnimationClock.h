// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "AVDexedTheme.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

/** One clock for every animation of the editor, on the screen's refresh
    (VBlankAttachment), at most 60 frames a second: whatever moves is updated
    in the same frame, instead of timers at 50 and 60 Hz that each woke the
    editor and asked for paints of their own. Nothing runs while nothing moves.
    A tick gets the frame's time (Time::getMillisecondCounterHiRes) and
    returns whether it still moves. */
class AVDexedAnimationClock {
public:
    using Tick = std::function<bool(double nowMs)>;

    explicit AVDexedAnimationClock(juce::Component& host)
        : state(std::make_shared<State>()),
          attachment(&host, [weak = std::weak_ptr<State>(state)](double displaySeconds) {
              if (auto locked = weak.lock()) locked->frame(displaySeconds);
          }) {}

    struct State {
        struct Entry {
            const void* owner;
            Tick tick;
            bool stopped = false;
        };
        std::vector<std::unique_ptr<Entry>> entries;
        bool inFrame = false;
        double lastFrame = -1.0;

        void run(const void* owner, Tick tick) {
            for (auto& entry : entries)
                if (entry->owner == owner) entry->stopped = true;
            entries.push_back(std::make_unique<Entry>(Entry { owner, std::move(tick) }));
            sweep();
        }
        void stop(const void* owner) {
            for (auto& entry : entries)
                if (entry->owner == owner) entry->stopped = true;
            sweep();
        }
        bool isRunning(const void* owner) const {
            for (auto& entry : entries)
                if (entry->owner == owner && !entry->stopped) return true;
            return false;
        }
        void frame(double displaySeconds) {
            if (entries.empty()) return;
            // 120 Hz (and faster) screens: every other refresh or so, as 60 Hz;
            // in low power, 30 Hz. The screen's own frame times are regular;
            // the message queue's are not.
            const double shortest = AVDexedTheme::lowPower ? 0.030 : 0.0125;
            if (lastFrame >= 0.0 && displaySeconds - lastFrame < shortest && displaySeconds >= lastFrame) return;
            lastFrame = displaySeconds;
            const double now = juce::Time::getMillisecondCounterHiRes();
            // A tick may start or stop others: entries are only marked during
            // the frame and removed after it; ones started now wait a frame.
            inFrame = true;
            const size_t count = entries.size();
            for (size_t i = 0; i < count; ++i) {
                auto* entry = entries[i].get();
                if (!entry->stopped && !entry->tick(now)) entry->stopped = true;
            }
            inFrame = false;
            sweep();
        }
        void sweep() {
            if (inFrame) return;
            entries.erase(std::remove_if(entries.begin(), entries.end(), [](const auto& entry) { return entry->stopped; }),
                          entries.end());
        }
    };

    std::shared_ptr<State> state;

private:
    juce::VBlankAttachment attachment;
};

/** Implemented by the editor: the clock its components share. */
struct AVDexedAnimationHost {
    virtual ~AVDexedAnimationHost() = default;
    virtual AVDexedAnimationClock& getAnimationClock() = 0;
};

/** One component's animation: on the editor's clock when the component is
    inside it, else on its own 60 Hz timer (the tests, or before it is placed). */
class AVDexedAnimation : private juce::Timer {
public:
    AVDexedAnimation(juce::Component& owner, AVDexedAnimationClock::Tick tick)
        : component(owner), onFrame(std::move(tick)) {}
    ~AVDexedAnimation() override { stop(); }

    /** Runs the tick every frame until it returns false or stop() is called. */
    void start() {
        if (isRunning()) return;
        auto* host = dynamic_cast<AVDexedAnimationHost*>(&component);
        if (host == nullptr) host = component.findParentComponentOfClass<AVDexedAnimationHost>();
        if (host == nullptr) {
            startTimerHz(60);
            return;
        }
        clock = host->getAnimationClock().state;
        clock.lock()->run(this, [this](double now) { return onFrame(now); });
    }
    void stop() {
        stopTimer();
        if (auto locked = clock.lock()) locked->stop(this);
        clock.reset();
    }
    bool isRunning() const {
        if (isTimerRunning()) return true;
        auto locked = clock.lock();
        return locked != nullptr && locked->isRunning(this);
    }

private:
    void timerCallback() override {
        if (!onFrame(juce::Time::getMillisecondCounterHiRes())) stopTimer();
    }
    juce::Component& component;
    AVDexedAnimationClock::Tick onFrame;
    std::weak_ptr<AVDexedAnimationClock::State> clock;
};
