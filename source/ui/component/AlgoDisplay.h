/**
 *
 * Copyright (c) 2014-2025 Pascal Gauthier.
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
 */

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <functional>
#include "parameter/DexedApvts.h"
#include "parameter/Model.h"
#include "../util/CachedParameter.h"
#include "../AVDexedPixelCache.h"

#include "../../debugger/Logger.h"

class AlgoDisplay : public juce::Component, public juce::SettableTooltipClient, private juce::Timer {
    DexedApvts& state;
    CachedParameter<int> algoValue;
    CachedParameter<bool> opSwitch[6];
    // Hero style (Play): bigger nodes, thin links, carriers in ocean, nodes that
    // light up with their live output and can be clicked.
    bool hero = false;
    // Glow: the processor's levels are targets; what is drawn eases toward
    // them (quick fade-in, slower fade-out) so it never pops on or off.
    std::array<float, 6> levels {};
    std::array<float, 6> levelTargets {};
    double lastGlowTick = 0.0;
    void timerCallback() override;
    /** Repaints only the node `op` and its glow: the rest of the diagram is
        unchanged by a level (paint skips what lies outside that area). */
    void repaintGlow(size_t op);
    /** The glow's strength as paint draws it (0 when the operator is off). */
    float glowOf(size_t op) const;
    /** What the glow of `op` puts on screen: -1 (none) or its two fills'
        alpha bytes. Most frames move a level by less than one byte: those
        are not repainted, which changes no pixel. */
    int glowPixels(size_t op) const;
    std::array<int, 6> glowOnScreen { -1, -1, -1, -1, -1, -1 };
    std::array<juce::Rectangle<float>, 6> opBounds {};
    juce::AffineTransform heroTransform;
    int hoveredOp = -1;
    // Play: the empty grid and each operator's body and wires, as images at
    // the screen's pixels; a glow frame draws them back around the glows.
    AVDexedPixelCache gridLayer;
    std::array<AVDexedPixelCache, 6> opLayers;
    void invalidateLayers() {
        gridLayer.invalidate();
        for (auto& layer : opLayers) layer.invalidate();
    }

    void displayOp(juce::Graphics &g, char id, int x, int y, char link, char fb);
    int operatorAt(juce::Point<float> position) const;
    juce::String frequencyText(int op) const;
    // Each operator's frequency parameters, looked up once; the text is made
    // again only when one of them changes (paint runs on every glow frame).
    struct FrequencyLabel {
        std::atomic<float>* coarse = nullptr;
        std::atomic<float>* fine = nullptr;
        std::atomic<float>* fixed = nullptr;
        float coarseShown = 0.0f, fineShown = 0.0f, fixedShown = 0.0f;
        juce::String text;
    };
    mutable std::array<FrequencyLabel, 6> frequencyLabels;
public:
    AlgoDisplay(DexedApvts &apvts) : state(apvts) {
        algoValue.referTo(apvts, IDs::algorithm.name);
        algoValue.callback = [this]() {
            invalidateLayers();
            repaint();
        };

        for (int i=0;i<6;i++) {
            opSwitch[i].referTo(apvts, IDs::on.op(i).name);
            opSwitch[i].callback = [this]() {
                invalidateLayers();
                repaint();
            };
            auto& label = frequencyLabels[size_t(i)];
            label.coarse = apvts.getRawParameterValue(IDs::frequencyCoarse.op(i).name);
            label.fine = apvts.getRawParameterValue(IDs::frequencyFine.op(i).name);
            label.fixed = apvts.getRawParameterValue(IDs::mode.op(i).name);
        }
    }

    void paint(juce::Graphics &g) override;
    void resized() override { invalidateLayers(); }
    void setHeroStyle(bool shouldUseHeroStyle);
    /** Normalised operator output levels (0..1), op 1 first. */
    void setLevels(const std::array<float, 6>& newLevels);
    int getAlgorithm() const { return algoValue; }
    std::function<void(int)> onOperatorClicked;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
};
