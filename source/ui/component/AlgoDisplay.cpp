/**
 *
 * Copyright (c) 2014, 2025 Pascal Gauthier.
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

#include "AlgoDisplay.h"
#include <cmath>
#include <vector>
#include "../DXLookNFeel.h"
#include "../AVDexedTheme.h"
#include "../AVDexedLanguage.h"

void AlgoDisplay::displayOp(juce::Graphics &g, char id, int x, int y, char link, char fb) {
    const float LINE_SZ = hero ? 1.1f : 2.2f;
    const String t = String(int(id));
    bool opOn = opSwitch[id-1];
    const bool carrier = y == 3;

    x *= 25;
    x += 3;
    y *= 21;
    y += 5;

    // Wires are continuous polylines whose segments meet exactly (the old
    // per-segment drawLine calls were off by one unit at every bend), with a
    // small decorative dot on every bend and junction.
    const float cx = float(x) + 8.0f;        // node centre line
    const float bottom = float(y) + 13.0f;   // node bottom edge
    const float nextTop = float(y) + 20.0f;  // top edge of the node one row down
    const float bus = float(y) + 17.5f;      // horizontal routing level
    const float lift = float(y) - 4.5f;      // feedback loop above the node
    const auto loopAt = [y](int rowsDown) { return float(y + 21 * rowsDown) + 15.5f; };
    Path wires;
    std::vector<Point<float>> joints;
    const auto poly = [&wires, &joints](std::initializer_list<Point<float>> points) {
        auto it = points.begin();
        wires.startNewSubPath(*it);
        for (++it; it != points.end(); ++it) {
            wires.lineTo(*it);
            if (std::next(it) != points.end()) joints.push_back(*it);   // bends
        }
    };

    switch(link) {
        case 0 : // LINE DOWN (a carrier has no node below: it stops at the bus)
            poly({{cx, bottom}, {cx, carrier ? bus : nextTop}});
            break;
        case 1: // ARROW TO RIGHT
            poly({{cx, bottom}, {cx, bus}, {cx + 25.0f, bus}});
            joints.push_back({cx + 25.0f, bus});
            break;
        case 2: // ARROW TO RIGHT JOIN
            poly({{cx, bottom}, {cx, bus}});
            break;
        case 3: // ARROW TO RIGHT AND DOWN
            poly({{cx, bottom}, {cx, nextTop}});
            poly({{cx, bus}, {cx + 25.0f, bus}, {cx + 25.0f, nextTop}});
            joints.push_back({cx, bus});
            break;
        case 4: // ARROW TO RIGHT+LEFT AND DOWN
            poly({{cx, bottom}, {cx, nextTop}});
            poly({{cx - 25.0f, nextTop}, {cx - 25.0f, bus}, {cx + 25.0f, bus}, {cx + 25.0f, nextTop}});
            joints.push_back({cx, bus});
            break;
        case 6:
            poly({{cx, bottom}, {cx, bus}, {cx + 50.0f, bus}});
            joints.push_back({cx + 50.0f, bus});
            break;
        case 7: // ARROW TO LEFT
            poly({{cx, bottom}, {cx, bus}, {cx - 25.0f, bus}});
            joints.push_back({cx - 25.0f, bus});
            break;
    }

    // Feedback: from the operator's output back over the top into its input.
    const float right = float(x) + 20.5f, left = float(x) - 4.5f;   // same 3.5 gap both sides
    switch(fb) {
        case 0:
            break;
        case 1:
            poly({{cx, float(y) - 1.0f}, {cx, lift}, {right, lift}, {right, loopAt(0)}, {cx, loopAt(0)}});
            joints.push_back({cx, loopAt(0)});
            break;
        case 2: // ALGO 4
            poly({{cx, float(y) - 1.0f}, {cx, lift}, {right, lift}, {right, loopAt(2)}, {cx, loopAt(2)}});
            joints.push_back({cx, loopAt(2)});
            break;
        case 3: // ALGO 6
            poly({{cx, float(y) - 1.0f}, {cx, lift}, {right, lift}, {right, loopAt(1)}, {cx, loopAt(1)}});
            joints.push_back({cx, loopAt(1)});
            break;
        case 4:
            poly({{cx, float(y) - 1.0f}, {cx, lift}, {left, lift}, {left, loopAt(0)}, {cx, loopAt(0)}});
            joints.push_back({cx, loopAt(0)});
            break;
    }

    // Dots are solid, in exactly the colour of their wire. Every dot sits on a
    // vertex of the wires, so the wires' bounds (with the pen and a dot) hold
    // everything they draw.
    const float dot = LINE_SZ * (hero ? 2.4f : 1.5f);
    const auto wireArea = wires.getBounds().expanded(dot + LINE_SZ);
    const auto wireColour = opOn ? (hero ? AVDexedTheme::oceanTop : DXLookNFeel::fillColour) : AVDexedTheme::outline;
    const auto drawWires = [&](Graphics& wg) {
        wg.setColour(wireColour);
        wg.strokePath(wires, PathStrokeType(LINE_SZ, PathStrokeType::mitered, PathStrokeType::butt));
        for (const auto& joint : joints)
            wg.fillEllipse(Rectangle<float>(dot, dot).withCentre(joint));
    };

    if (hero) {
        // `g` is in this component's coordinates; the diagram's units go
        // through heroTransform. The drawing order is the original one: the
        // glow, then the node's body and wires, then its two labels (which
        // no wire reaches).
        const auto node = Rectangle<float>(float(x) - 1.0f, float(y) - 1.0f, 18.0f, 14.0f);
        const size_t index = size_t(id - 1);
        opBounds[index] = node.transformedBy(heroTransform);
        // Opacity grows from exactly zero, so the glow fades rather than
        // switching on at a minimum strength.
        const float glow = opOn ? std::pow(levels[index], 0.7f) : 0.0f;
        if (glow > 0.002f && g.clipRegionIntersects(node.expanded(3.2f).transformedBy(heroTransform).getSmallestIntegerContainer())) {
            Graphics::ScopedSaveState state(g);
            g.addTransform(heroTransform);
            g.setColour(AVDexedTheme::oceanHighlight.withAlpha(0.32f * glow));
            g.fillRoundedRectangle(node.expanded(2.2f), 4.5f);
            g.setColour(AVDexedTheme::oceanHighlight.withAlpha(0.44f * glow));
            g.fillRoundedRectangle(node.expanded(1.0f), 3.5f);
        }
        // The body and the wires change only with the algorithm, a switch or
        // the hover: kept as an image at the screen's pixels.
        const bool hovered = hoveredOp == id - 1;
        const auto layerArea = node.expanded(1.0f).getUnion(wireArea).transformedBy(heroTransform).expanded(1.0f);
        opLayers[index].draw(g, *this, layerArea, [&](Graphics& lg) {
            lg.addTransform(heroTransform);
            if (carrier && opOn)
                lg.setGradientFill(ColourGradient(AVDexedTheme::oceanTop, 0.0f, node.getY(),
                                                  AVDexedTheme::oceanBottom, 0.0f, node.getBottom(), false));
            else
                lg.setGradientFill(ColourGradient(opOn ? AVDexedTheme::outline : AVDexedTheme::controlTop, 0.0f, node.getY(),
                                                  AVDexedTheme::controlBottom, 0.0f, node.getBottom(), false));
            lg.fillRoundedRectangle(node, 3.0f);
            lg.setColour(hovered ? AVDexedTheme::textPrimary
                                 : opOn ? AVDexedTheme::oceanHighlight.withAlpha(carrier ? 0.9f : 0.55f)
                                        : AVDexedTheme::outline);
            lg.drawRoundedRectangle(node.reduced(0.25f), 3.0f, hovered ? 0.55f : 0.4f);
            drawWires(lg);
        });
        // A glow frame repaints only nodes' areas: labels wholly outside the
        // area being painted would change no pixel, so they are skipped.
        if (g.clipRegionIntersects(opBounds[index].getSmallestIntegerContainer())) {
            Graphics::ScopedSaveState state(g);
            g.addTransform(heroTransform);
            auto label = node;
            const auto ratio = label.removeFromBottom(5.2f).translated(0.0f, -0.6f);
            g.setFont(AVDexedTheme::font(6.6f, "SemiBold"));
            g.setColour(opOn ? AVDexedTheme::textPrimary : AVDexedTheme::textSecondary.withAlpha(0.55f));
            g.drawText(t, label.translated(0.0f, 0.4f), Justification::centred, false);
            // Frequency ratio (or fixed Hz): the number that shapes the timbre most.
            g.setFont(AVDexedTheme::font(3.3f, "Medium"));
            g.setColour((carrier && opOn ? AVDexedTheme::textPrimary : AVDexedTheme::textSecondary).withAlpha(opOn ? 0.85f : 0.45f));
            g.drawText(frequencyText(id - 1), ratio, Justification::centred, false);
        }
        return;
    }

    // Each operator sits in its own grid cell, like the classic panel. A muted
    // operator stays readable (dimmed) instead of vanishing into the panel.
    const auto box = Rectangle<float>(float(x) - 1.0f, float(y) - 1.0f, 18.0f, 14.0f);
    g.setGradientFill(ColourGradient(opOn ? AVDexedTheme::outline : AVDexedTheme::controlTop, 0.0f, box.getY(),
                                     AVDexedTheme::controlBottom, 0.0f, box.getBottom(), false));
    g.fillRoundedRectangle(box, 2.5f);
    g.setColour(opOn ? AVDexedTheme::oceanHighlight.withAlpha(0.75f) : AVDexedTheme::outline.withAlpha(0.8f));
    g.drawRoundedRectangle(box.reduced(0.4f), 2.5f, 0.8f);
    g.setColour(opOn ? AVDexedTheme::textPrimary : AVDexedTheme::textSecondary.withAlpha(0.6f));
    g.drawText(t, x, y, 16, 12, Justification::centred, true);
    drawWires(g);
}

void AlgoDisplay::paint(Graphics &g) {
    const float scale = jmin(float(getWidth()) / 152.0f, float(getHeight()) / 91.0f);
    heroTransform = AffineTransform::scale(scale).translated((getWidth() - 152 * scale) * (hero ? 0.5f : 0.0f),
                                                             (getHeight() - 91 * scale) * 0.5f);
    // Empty 6×4 grid of the classic algorithm panel; operators fill their cells.
    const auto drawGrid = [this](Graphics& gg) {
        gg.setColour(AVDexedTheme::track.withAlpha(hero ? 0.13f : 0.6f));
        for (int column = 0; column < 6; ++column)
            for (int row = 0; row < 4; ++row)
                gg.fillRoundedRectangle(float(column * 25 + 2), float(row * 21 + 4), 18.0f, 14.0f, hero ? 3.0f : 2.5f);
    };
    int algo = algoValue;
    if (hero) {
        // Play: the grid never changes, so it is an image at the screen's pixels.
        gridLayer.draw(g, *this, getLocalBounds().toFloat(), [this, &drawGrid](Graphics& lg) {
            lg.addTransform(heroTransform);
            drawGrid(lg);
        });
    } else {
        g.addTransform(heroTransform);
        g.setFont(AVDexedTheme::font(11.5f, "SemiBold"));
        drawGrid(g);
        g.setColour(DXLookNFeel::fillColour);
        g.fillRoundedRectangle(1.0f, 3.0f, 20.0f, 15.0f, 2.5f);
        g.setColour(Colours::white);
        g.drawText(juce::String(algo), 1, 3, 20, 15, Justification::centred, true);
    }
    
    switch(algo-1) {
        case 0:
            displayOp(g, 6, 3, 0, 0, 1);
            displayOp(g, 5, 3, 1, 0, 0);
            displayOp(g, 4, 3, 2, 0, 0);
            displayOp(g, 3, 3, 3, 2, 0);
            displayOp(g, 2, 2, 2, 0, 0);
            displayOp(g, 1, 2, 3, 1, 0);
            break;
        case 1:
            displayOp(g, 6, 3, 0, 0, 0);
            displayOp(g, 5, 3, 1, 0, 0);
            displayOp(g, 4, 3, 2, 0, 0);
            displayOp(g, 3, 3, 3, 2, 0);
            displayOp(g, 2, 2, 2, 0, 1);
            displayOp(g, 1, 2, 3, 1, 0);
            break;
        case 2:
            displayOp(g, 6, 3, 1, 0, 1);
            displayOp(g, 5, 3, 2, 0, 0);
            displayOp(g, 4, 3, 3, 2, 0);
            displayOp(g, 3, 2, 1, 0, 0);
            displayOp(g, 2, 2, 2, 0, 0);
            displayOp(g, 1, 2, 3, 1, 0);
            break;
        case 3: //
            displayOp(g, 6, 3, 1, 0, 2);
            displayOp(g, 5, 3, 2, 0, 0);
            displayOp(g, 4, 3, 3, 2, 0);
            displayOp(g, 3, 2, 1, 0, 0);
            displayOp(g, 2, 2, 2, 0, 0);
            displayOp(g, 1, 2, 3, 1, 0);
            break;
        case 4:
            displayOp(g, 6, 4, 2, 0, 1);
            displayOp(g, 5, 4, 3, 2, 0);
            displayOp(g, 4, 3, 2, 0, 0);
            displayOp(g, 3, 3, 3, 1, 0);
            displayOp(g, 2, 2, 2, 0, 0);
            displayOp(g, 1, 2, 3, 1, 0);
            break;
        case 5: //
            displayOp(g, 6, 4, 2, 0, 3);
            displayOp(g, 5, 4, 3, 2, 0);
            displayOp(g, 4, 3, 2, 0, 0);
            displayOp(g, 3, 3, 3, 1, 0);
            displayOp(g, 2, 2, 2, 0, 0);
            displayOp(g, 1, 2, 3, 1, 0);
            break;
        case 6:
            displayOp(g, 6, 4, 1, 0, 1);
            displayOp(g, 5, 4, 2, 7, 0);
            displayOp(g, 4, 3, 2, 0, 0);
            displayOp(g, 3, 3, 3, 2, 0);
            displayOp(g, 2, 2, 2, 0, 0);
            displayOp(g, 1, 2, 3, 1, 0);
            break;
        case 7:
            displayOp(g, 6, 4, 1, 0, 0);
            displayOp(g, 5, 4, 2, 7, 0);
            displayOp(g, 4, 3, 2, 0, 4);
            displayOp(g, 3, 3, 3, 2, 0);
            displayOp(g, 2, 2, 2, 0, 0);
            displayOp(g, 1, 2, 3, 1, 0);
            break;
        case 8:
            displayOp(g, 6, 4, 1, 0, 0);
            displayOp(g, 5, 4, 2, 7, 0);
            displayOp(g, 4, 3, 2, 0, 0);
            displayOp(g, 3, 3, 3, 2, 0);
            displayOp(g, 2, 2, 2, 0, 1);
            displayOp(g, 1, 2, 3, 1, 0);
            break;
        case 9:
            displayOp(g, 6, 2, 2, 0, 0);
            displayOp(g, 5, 1, 2, 1, 0);
            displayOp(g, 4, 2, 3, 1, 0);
            displayOp(g, 3, 3, 1, 0, 1);
            displayOp(g, 2, 3, 2, 0, 0);
            displayOp(g, 1, 3, 3, 2, 0);
            break;
        case 10:
            displayOp(g, 6, 2, 2, 0, 1);
            displayOp(g, 5, 1, 2, 1, 0);
            displayOp(g, 4, 2, 3, 1, 0);
            displayOp(g, 3, 3, 1, 0, 0);
            displayOp(g, 2, 3, 2, 0, 0);
            displayOp(g, 1, 3, 3, 2, 0);
            break;
        case 11:
            displayOp(g, 6, 3, 2, 7, 0);
            displayOp(g, 5, 2, 2, 0, 0);
            displayOp(g, 4, 1, 2, 1, 0);
            displayOp(g, 3, 2, 3, 6, 0);
            displayOp(g, 2, 4, 2, 0, 1);
            displayOp(g, 1, 4, 3, 2, 0);
            break;
        case 12:
            displayOp(g, 6, 3, 2, 7, 1);
            displayOp(g, 5, 2, 2, 0, 0);
            displayOp(g, 4, 1, 2, 1, 0);
            displayOp(g, 3, 2, 3, 6, 0);
            displayOp(g, 2, 4, 2, 0, 0);
            displayOp(g, 1, 4, 3, 2, 0);
            break;
        case 13:                                                                       
            displayOp(g, 6, 4, 1, 7, 1);// displayOp(g, 6, 3, 1, 0, 1);
            displayOp(g, 5, 3, 1, 0, 0);// displayOp(g, 5, 2, 1, 1, 0);
            displayOp(g, 4, 3, 2, 0, 0);
            displayOp(g, 3, 3, 3, 2, 0);
            displayOp(g, 2, 2, 2, 0, 0);
            displayOp(g, 1, 2, 3, 1, 0);
            break;
        case 14:
            displayOp(g, 6, 4, 1, 7, 0);// displayOp(g, 6, 3, 1, 0, 0);
            displayOp(g, 5, 3, 1, 0, 0);// displayOp(g, 5, 2, 1, 1, 0);
            displayOp(g, 4, 3, 2, 0, 0);
            displayOp(g, 3, 3, 3, 2, 0);
            displayOp(g, 2, 2, 2, 0, 4);
            displayOp(g, 1, 2, 3, 1, 0);
            break;
        case 15:
            displayOp(g, 6, 4, 1, 0, 1);
            displayOp(g, 5, 4, 2, 7, 0);
            displayOp(g, 4, 3, 1, 0, 0);
            displayOp(g, 3, 3, 2, 0, 0);
            displayOp(g, 2, 2, 2, 1, 0);
            displayOp(g, 1, 3, 3, 0, 0);
            break;
        case 16:
            displayOp(g, 6, 4, 1, 0, 0);
            displayOp(g, 5, 4, 2, 7, 0);
            displayOp(g, 4, 3, 1, 0, 0);
            displayOp(g, 3, 3, 2, 0, 0);
            displayOp(g, 2, 2, 2, 1, 4);
            displayOp(g, 1, 3, 3, 0, 0);
            break;
        case 17:
            displayOp(g, 6, 4, 0, 0, 0);
            displayOp(g, 5, 4, 1, 0, 0);
            displayOp(g, 4, 4, 2, 7, 0);
            displayOp(g, 3, 3, 2, 0, 4);
            displayOp(g, 2, 2, 2, 1, 0);
            displayOp(g, 1, 3, 3, 0, 0);
            break;
        case 18:
            displayOp(g, 6, 3, 2, 3, 1);
            displayOp(g, 5, 4, 3, 2, 0);
            displayOp(g, 4, 3, 3, 1, 0);
            displayOp(g, 3, 2, 1, 0, 0);
            displayOp(g, 2, 2, 2, 0, 0);
            displayOp(g, 1, 2, 3, 1, 0);
            break;
        case 19:
            displayOp(g, 6, 4, 2, 0, 0);
            displayOp(g, 5, 3, 2, 1, 0);
            displayOp(g, 4, 4, 3, 2, 0);
            displayOp(g, 3, 1, 2, 3, 1);
            displayOp(g, 2, 2, 3, 6, 0);
            displayOp(g, 1, 1, 3, 1, 0);
            break;
        case 20:
            displayOp(g, 6, 3, 2, 3, 0);
            displayOp(g, 5, 4, 3, 2, 0);
            displayOp(g, 4, 3, 3, 1, 0);
            displayOp(g, 3, 1, 2, 3, 1);
            displayOp(g, 2, 2, 3, 1, 0);
            displayOp(g, 1, 1, 3, 1, 0);
            break;
        case 21:
            displayOp(g, 6, 3, 2, 4, 1);
            displayOp(g, 5, 4, 3, 2, 0);
            displayOp(g, 4, 3, 3, 1, 0);
            displayOp(g, 3, 2, 3, 1, 0);
            displayOp(g, 2, 1, 2, 0, 0);
            displayOp(g, 1, 1, 3, 1, 0);
            break;
        case 22: // CC
            displayOp(g, 6, 3, 2, 3, 1);
            displayOp(g, 5, 4, 3, 2, 0);
            displayOp(g, 4, 3, 3, 1, 0);
            displayOp(g, 3, 2, 2, 0, 0);
            displayOp(g, 2, 2, 3, 1, 0);
            displayOp(g, 1, 1, 3, 1, 0);
            break;
        case 23: // CC
            displayOp(g, 6, 3, 2, 4, 1);
            displayOp(g, 5, 4, 3, 2, 0);
            displayOp(g, 4, 3, 3, 1, 0);
            displayOp(g, 3, 2, 3, 1, 0);
            displayOp(g, 2, 1, 3, 1, 0);
            displayOp(g, 1, 0, 3, 1, 0);
            break;
        case 24: // CC
            displayOp(g, 6, 3, 2, 3, 1);
            displayOp(g, 5, 4, 3, 2, 0);
            displayOp(g, 4, 3, 3, 1, 0);
            displayOp(g, 3, 2, 3, 1, 0);
            displayOp(g, 2, 1, 3, 1, 0);
            displayOp(g, 1, 0, 3, 1, 0);
            break;
        case 25:
            displayOp(g, 6, 4, 2, 0, 1);
            displayOp(g, 5, 3, 2, 1, 0);
            displayOp(g, 4, 4, 3, 2, 0);
            displayOp(g, 3, 2, 2, 0, 0);
            displayOp(g, 2, 2, 3, 6, 0);
            displayOp(g, 1, 1, 3, 1, 0);
            break;
        case 26:
            displayOp(g, 6, 4, 2, 0, 0);
            displayOp(g, 5, 3, 2, 1, 0);
            displayOp(g, 4, 4, 3, 2, 0);
            displayOp(g, 3, 2, 2, 0, 1);
            displayOp(g, 2, 2, 3, 6, 0);
            displayOp(g, 1, 1, 3, 1, 0);
            break;
        case 27:
            displayOp(g, 6, 4, 3, 2, 0);
            displayOp(g, 5, 3, 1, 0, 1);
            displayOp(g, 4, 3, 2, 0, 0);
            displayOp(g, 3, 3, 3, 1, 0);
            displayOp(g, 2, 2, 2, 0, 0);
            displayOp(g, 1, 2, 3, 1, 0);
            break;
        case 28:
            displayOp(g, 6, 4, 2, 0, 1);
            displayOp(g, 5, 4, 3, 2, 0);
            displayOp(g, 4, 3, 2, 0, 0);
            displayOp(g, 3, 3, 3, 1, 0);
            displayOp(g, 2, 2, 3, 1, 0);
            displayOp(g, 1, 1, 3, 1, 0);
            break;
        case 29:
            displayOp(g, 6, 4, 3, 2, 0);
            displayOp(g, 5, 3, 1, 0, 1);
            displayOp(g, 4, 3, 2, 0, 0);
            displayOp(g, 3, 3, 3, 1, 0);
            displayOp(g, 2, 2, 3, 1, 0);
            displayOp(g, 1, 1, 3, 1, 0);
            break;
        case 30:
            displayOp(g, 6, 4, 2, 0, 1);
            displayOp(g, 5, 4, 3, 2, 0);
            displayOp(g, 4, 3, 3, 1, 0);
            displayOp(g, 3, 2, 3, 1, 0);
            displayOp(g, 2, 1, 3, 1, 0);
            displayOp(g, 1, 0, 3, 1, 0);
            break;
        case 31:
            displayOp(g, 6, 5, 3, 2, 1);
            displayOp(g, 5, 4, 3, 1, 0);
            displayOp(g, 4, 3, 3, 1, 0);
            displayOp(g, 3, 2, 3, 1, 0);
            displayOp(g, 2, 1, 3, 1, 0);
            displayOp(g, 1, 0, 3, 1, 0);       
            break;
        default:
            break;
    }
}


void AlgoDisplay::setHeroStyle(bool shouldUseHeroStyle) {
    hero = shouldUseHeroStyle;
    setInterceptsMouseClicks(hero, false);
    invalidateLayers();
    repaint();
}

void AlgoDisplay::setLevels(const std::array<float, 6>& newLevels) {
    bool moving = false;
    for (size_t i = 0; i < levels.size(); ++i) {
        levelTargets[i] = jlimit(0.0f, 1.0f, newLevels[i]);
        moving |= std::abs(levelTargets[i] - levels[i]) > 0.002f;
    }
    if (moving && hero && !isTimerRunning()) {
        lastGlowTick = Time::getMillisecondCounterHiRes();
        startTimerHz(60);
    }
}

void AlgoDisplay::timerCallback() {
    // Exponential ease with time constants of 45 ms rising and 140 ms
    // falling: about 100 ms to fade in and 400 ms to fade out.
    const double now = Time::getMillisecondCounterHiRes();
    const float elapsed = float(jlimit(1.0, 100.0, now - lastGlowTick));
    lastGlowTick = now;
    const float rise = 1.0f - std::exp(-elapsed / 45.0f);
    const float fall = 1.0f - std::exp(-elapsed / 140.0f);
    bool moving = false;
    for (size_t i = 0; i < levels.size(); ++i) {
        const float target = levelTargets[i];
        float next = levels[i] + (target - levels[i]) * (target > levels[i] ? rise : fall);
        if (std::abs(target - next) <= 0.002f) next = target;
        else moving = true;
        // A muted operator never glows: its level changes nothing on screen.
        if (next != levels[i] && opSwitch[i]) repaintGlow(i);
        levels[i] = next;
    }
    if (!moving) stopTimer();
}

void AlgoDisplay::repaintGlow(size_t op) {
    // Not painted yet (no node areas known): the whole display.
    if (opBounds[op].isEmpty()) {
        repaint();
        return;
    }
    // The glow reaches 2.2 units past the node; one more unit keeps the edge's smoothing.
    const float unit = opBounds[op].getWidth() / 18.0f;
    repaint(opBounds[op].expanded(3.2f * unit).getSmallestIntegerContainer());
}

int AlgoDisplay::operatorAt(Point<float> position) const {
    for (size_t i = 0; i < opBounds.size(); ++i)
        if (opBounds[i].expanded(4.0f).contains(position))
            return int(i);
    return -1;
}

void AlgoDisplay::mouseMove(const MouseEvent& event) {
    if (!hero) return;
    const int op = operatorAt(event.position);
    if (op == hoveredOp) return;
    hoveredOp = op;
    invalidateLayers();   // the hovered node's outline
    setMouseCursor(op >= 0 ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor);
    setTooltip(op >= 0 ? AVDexedLanguageService::getInstance().format("operatorOpen", {{"number", String(op + 1)}}) : String());
    repaint();
}

void AlgoDisplay::mouseExit(const MouseEvent&) {
    if (hoveredOp < 0) return;
    hoveredOp = -1;
    invalidateLayers();
    setTooltip({});
    repaint();
}

void AlgoDisplay::mouseUp(const MouseEvent& event) {
    if (!hero || event.mouseWasDraggedSinceMouseDown()) return;
    const int op = operatorAt(event.position);
    if (op >= 0 && onOperatorClicked) onOperatorClicked(op);
}

String AlgoDisplay::frequencyText(int op) const {
    auto& label = frequencyLabels[size_t(op)];
    const auto value = [](const std::atomic<float>* parameter) { return parameter != nullptr ? parameter->load() : 0.0f; };
    const float coarseValue = value(label.coarse), fineValue = value(label.fine), fixedValue = value(label.fixed);
    if (label.text.isNotEmpty() && coarseValue == label.coarseShown && fineValue == label.fineShown && fixedValue == label.fixedShown)
        return label.text;
    label.coarseShown = coarseValue;
    label.fineShown = fineValue;
    label.fixedShown = fixedValue;
    const float coarse = std::round(coarseValue);
    const float fine = std::round(fineValue);
    if (fixedValue >= 0.5f) {
        const double hz = std::pow(10.0, int(coarse) & 3) * std::exp(2.30258509299404568402 * fine / 100.0);
        // String(double, 0) keeps every digit in JUCE, so whole hertz are rounded explicitly.
        label.text = (hz < 100.0 ? String(hz, hz < 10.0 ? 2 : 1) : String(roundToInt(hz))) + " Hz";
        return label.text;
    }
    const float ratio = (coarse == 0.0f ? 0.5f : coarse) * (1.0f + fine / 100.0f);
    auto text = String(ratio, 2);
    while (text.containsChar('.') && (text.endsWithChar('0') || text.endsWithChar('.')))
        text = text.dropLastCharacters(1);
    label.text = String::fromUTF8("×") + text;
    return label.text;
}
