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
    std::array<juce::Rectangle<float>, 6> opBounds {};
    juce::AffineTransform heroTransform;
    int hoveredOp = -1;

    void displayOp(juce::Graphics &g, char id, int x, int y, char link, char fb);
    int operatorAt(juce::Point<float> position) const;
    juce::String frequencyText(int op) const;
public:
    AlgoDisplay(DexedApvts &apvts) : state(apvts) {
        algoValue.referTo(apvts, IDs::algorithm.name);
        algoValue.callback = [this]() {
            repaint();
        };

        for (int i=0;i<6;i++) {
            opSwitch[i].referTo(apvts, IDs::on.op(i).name);
            opSwitch[i].callback = [this]() {
                repaint();
            };
        }
    }

    void paint(juce::Graphics &g) override;
    void setHeroStyle(bool shouldUseHeroStyle);
    /** Normalised operator output levels (0..1), op 1 first. */
    void setLevels(const std::array<float, 6>& newLevels);
    int getAlgorithm() const { return algoValue; }
    std::function<void(int)> onOperatorClicked;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
};
