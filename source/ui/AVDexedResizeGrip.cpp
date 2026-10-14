// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedResizeGrip.h"
#include "AVDexedTheme.h"
#include "../JuceLibraryCode/BinaryData.h"

using namespace juce;

AVDexedResizeGrip::AVDexedResizeGrip() {
    setMouseCursor(MouseCursor::BottomRightCornerResizeCursor);
    setWantsKeyboardFocus(false);
    setTitle("Resize editor");
    if (auto svg = XmlDocument::parse(String::fromUTF8(BinaryData::AVDexed_ResizeIcon_svg,
                                                        BinaryData::AVDexed_ResizeIcon_svgSize)))
        icon = Drawable::createFromSVG(*svg);
}

void AVDexedResizeGrip::paint(Graphics& g) {
    if (icon != nullptr) {
        const auto area = getLocalBounds().toFloat();
        if (hovered || dragging) {
            auto path = icon->getOutlineAsPath();
            path.applyTransform(path.getTransformToScaleToFit(area, true));
            g.setGradientFill(ColourGradient(AVDexedTheme::oceanTop, 0, area.getY(),
                                             AVDexedTheme::oceanBottom, 0, area.getBottom(), false));
            g.fillPath(path);
        } else icon->drawWithin(g, area, RectanglePlacement::centred, 0.68f);
    }
}

void AVDexedResizeGrip::mouseEnter(const MouseEvent&) {
    hovered = true;
    repaint();
}

void AVDexedResizeGrip::mouseExit(const MouseEvent&) {
    hovered = false;
    repaint();
}

void AVDexedResizeGrip::mouseDown(const MouseEvent& event) {
    dragging = true;
    dragStartScreen = event.getScreenPosition();
    if (onResizeStart)
        onResizeStart();
}

void AVDexedResizeGrip::mouseDrag(const MouseEvent& event) {
    if (dragging && onResizeDrag)
        onResizeDrag(event.getScreenPosition() - dragStartScreen);
}

void AVDexedResizeGrip::mouseUp(const MouseEvent&) {
    dragging = false;
    repaint();
    if (onResizeEnd)
        onResizeEnd();
}
