// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <BinaryData.h>
#include <map>
#include <vector>

namespace AVDexedTheme {
inline const juce::Colour canvas { 0xff000000 };
inline const juce::Colour panelTop { 0xff10141c };
inline const juce::Colour panelBottom { 0xff070809 };
inline const juce::Colour textPrimary { 0xffe0e0e0 };
inline const juce::Colour textSecondary { 0xff9a9a9a };
inline const juce::Colour oceanTop { 0xff248fc7 };
inline const juce::Colour oceanBottom { 0xff0b3558 };
inline const juce::Colour oceanHighlight { 0xff58bfe8 };
inline const juce::Colour dangerTop { 0xffb10004 };
inline const juce::Colour dangerBottom { 0xff780002 };
inline const juce::Colour controlTop { 0xff17212a };
inline const juce::Colour controlBottom { 0xff080b0e };
inline const juce::Colour outline { 0xff35434d };
inline const juce::Colour track { 0xff26313a };
inline const juce::Colour banner { 0xff0b0d0f };
inline const juce::Colour silverTop { 0xff666666 };
inline const juce::Colour silverBottom { 0xff424242 };
inline const juce::Colour knobTop { 0xff121212 };
inline const juce::Colour knobBottom { 0xff242424 };
inline const juce::Colour dotTop { 0xff999999 };
inline const juce::Colour dotBottom { 0xffd2d2d2 };
inline juce::Typeface::Ptr typeface(const juce::String& style = "Medium") {
    // A name-based Font resolves via JUCE's global default LookAndFeel. Direct
    // embedded faces keep our font choice scoped to AV Dexed in every host.
    static const auto extraLight = juce::Typeface::createSystemTypefaceFor(BinaryData::MontserratExtraLight_ttf, BinaryData::MontserratExtraLight_ttfSize);
    static const auto light = juce::Typeface::createSystemTypefaceFor(BinaryData::MontserratLight_ttf, BinaryData::MontserratLight_ttfSize);
    static const auto regular = juce::Typeface::createSystemTypefaceFor(BinaryData::MontserratRegular_ttf, BinaryData::MontserratRegular_ttfSize);
    static const auto medium = juce::Typeface::createSystemTypefaceFor(BinaryData::MontserratMedium_ttf, BinaryData::MontserratMedium_ttfSize);
    static const auto semiBold = juce::Typeface::createSystemTypefaceFor(BinaryData::MontserratSemiBold_ttf, BinaryData::MontserratSemiBold_ttfSize);
    static const auto bold = juce::Typeface::createSystemTypefaceFor(BinaryData::MontserratBold_ttf, BinaryData::MontserratBold_ttfSize);
    if (style.containsIgnoreCase("ExtraLight")) return extraLight;
    if (style.containsIgnoreCase("Light")) return light;
    if (style.containsIgnoreCase("SemiBold")) return semiBold;
    if (style.containsIgnoreCase("Bold")) return bold;
    if (style.containsIgnoreCase("Medium")) return medium;
    return regular;
}
inline juce::Font font(float points, const juce::String& style = "Medium") {
    return juce::Font(juce::FontOptions(typeface(style)).withPointHeight(points)).withExtraKerningFactor(-0.04f);
}
/** Focus rings follow the web's :focus-visible rule: they appear after keyboard
    navigation and stay hidden after a mouse click, so clicking a knob or a tab
    leaves no outline behind. The editor turns them on with Tab and off on mouse down. */
inline bool& keyboardFocusVisible() {
    static bool visible = false;
    return visible;
}
inline bool showsFocus(const juce::Component& component) {
    return keyboardFocusVisible() && component.hasKeyboardFocus(true);
}
/** Turns focus rings on; true when that changed. For controls whose own keys
    move the focus (the bank manager's grid of 32 programs). */
inline bool revealKeyboardFocus() {
    if (keyboardFocusVisible()) return false;
    keyboardFocusVisible() = true;
    return true;
}
/** Called by every place that sees keys first (lists, dialogs, the editor).
    Only Tab (and Shift+Tab) counts as keyboard navigation: it is the key that
    moves the focus. Arrow keys choose sounds, nudge a value or move a list's
    selection; they never move the focus, so they must not show a ring on
    whatever control happens to hold it (the Play tab after a click on the top
    bar). Returns true when the rings were turned on. */
inline bool noteKeyboardNavigation(const juce::KeyPress& key) {
    return key.isKeyCode(juce::KeyPress::tabKey) && revealKeyboardFocus();
}
/** Key listener for components that consume navigation keys themselves. */
struct FocusKeyListener final : public juce::KeyListener {
    bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override {
        if (noteKeyboardNavigation(key) && origin != nullptr)
            if (auto* top = origin->getTopLevelComponent()) top->repaint();
        return false;
    }
};
/** Settings > Instrument > "Low power": animations at 30 frames a second,
    text in one solid tone, black keys without their shadow. Off by default. */
inline bool lowPower = false;
/** Vertical text fill from `top` to `bottom` over y1..y2. CoreGraphics paints a
    gradient glyph run as one clipped outline per glyph, and on 1x displays thin
    stems such as "l" or "-" can drop out of small text. Below ~16 device pixels
    the text uses one solid tone between both stops, which CoreText rasterises
    normally; larger text keeps the gradient (in low power, the solid tone too).
    Returns whether it is a gradient. */
inline bool textFill(juce::Graphics& g, const juce::Font& textFont, juce::Colour top, juce::Colour bottom,
                     float y1, float y2) {
    if (lowPower || textFont.getHeight() * g.getInternalContext().getPhysicalPixelScaleFactor() < 16.0f) {
        g.setColour(top.interpolatedWith(bottom, 0.4f));
        return false;
    }
    g.setGradientFill(juce::ColourGradient(top, 0.0f, y1, bottom, 0.0f, y2, false));
    return true;
}
/** Text measured and laid out between paints. Measuring a label (shaping) and,
    for gradient text on CoreGraphics, taking each glyph's outline from the font
    again cost more than filling it; both depend only on the text, the font and
    the box. The 512 most recent are kept (then it starts again). */
namespace TextCache {
/** Off: gradient text is drawn by Graphics::drawText (the tools compare both on screen). */
inline bool enabled = true;
/** Everything of a font that changes its glyphs, at full precision. */
inline juce::String key(const juce::Font& font, const juce::String& text) {
    return font.getTypefaceName() + "\n" + font.getTypefaceStyle() + "\n" + juce::String(font.getHeight(), 6) + "\n"
         + juce::String(font.getHorizontalScale(), 6) + "\n" + juce::String(font.getExtraKerningFactor(), 6) + "\n"
         + (font.isUnderlined() ? "u" : "") + "\n" + text;
}
inline float width(const juce::Font& font, const juce::String& text) {
    static std::map<juce::String, float> widths;
    auto name = key(font, text);
    if (const auto found = widths.find(name); found != widths.end()) return found->second;
    if (widths.size() >= 512) widths.clear();
    return widths[std::move(name)] = juce::GlyphArrangement::getStringWidth(font, text);
}
/** What GlyphArrangement::draw asks of a context, recorded: each run's font,
    glyphs and positions. Nothing is drawn. */
class GlyphRecorder final : public juce::LowLevelGraphicsContext {
public:
    struct Run {
        juce::Font font;
        std::vector<uint16_t> glyphs;
        std::vector<juce::Point<float>> positions;
    };
    std::vector<Run> runs;

    bool isVectorDevice() const override { return false; }
    void setOrigin(juce::Point<int>) override {}
    void addTransform(const juce::AffineTransform&) override {}
    float getPhysicalPixelScaleFactor() const override { return 1.0f; }
    bool clipToRectangle(const juce::Rectangle<int>&) override { return true; }
    bool clipToRectangleList(const juce::RectangleList<int>&) override { return true; }
    void excludeClipRectangle(const juce::Rectangle<int>&) override {}
    void clipToPath(const juce::Path&, const juce::AffineTransform&) override {}
    void clipToImageAlpha(const juce::Image&, const juce::AffineTransform&) override {}
    bool clipRegionIntersects(const juce::Rectangle<int>&) override { return true; }
    juce::Rectangle<int> getClipBounds() const override { return { -0x3fffffff, -0x3fffffff, 0x7ffffffe, 0x7ffffffe }; }
    bool isClipEmpty() const override { return false; }
    void saveState() override {}
    void restoreState() override {}
    void beginTransparencyLayer(float) override {}
    void endTransparencyLayer() override {}
    void setFill(const juce::FillType&) override {}
    void setOpacity(float) override {}
    void setInterpolationQuality(juce::Graphics::ResamplingQuality) override {}
    void fillRect(const juce::Rectangle<int>&, bool) override {}
    void fillRect(const juce::Rectangle<float>&) override {}
    void fillRectList(const juce::RectangleList<float>&) override {}
    void fillPath(const juce::Path&, const juce::AffineTransform&) override {}
    void drawImage(const juce::Image&, const juce::AffineTransform&) override {}
    void drawLine(const juce::Line<float>&) override {}
    void setFont(const juce::Font& newFont) override { font = newFont; }
    const juce::Font& getFont() override { return font; }
    void drawGlyphs(juce::Span<const uint16_t> glyphs, juce::Span<const juce::Point<float>> positions,
                    const juce::AffineTransform&) override {
        runs.push_back({ font, { glyphs.begin(), glyphs.end() }, { positions.begin(), positions.end() } });
    }
    std::unique_ptr<juce::ImageType> getPreferredImageTypeForTemporaryImages() const override {
        return std::make_unique<juce::SoftwareImageType>();
    }
    uint64_t getFrameId() const override { return 0; }

private:
    juce::Font font { juce::FontOptions {} };
};
/** A run of glyphs of one font, with each glyph's outline as the font gives it. */
struct GlyphRun {
    juce::Font font;
    std::vector<juce::Path> outlines;
    std::vector<juce::Point<float>> positions;
};
/** The glyphs Graphics::drawText (text, {0, 0, width, height}, ...) lays out
    (the same arrangement), with their outlines taken once. */
inline const std::vector<GlyphRun>& glyphRuns(const juce::Font& font, const juce::String& text, float width,
                                              float height, juce::Justification justification, bool ellipsis) {
    static std::map<juce::String, std::vector<GlyphRun>> layouts;
    auto name = key(font, text) + "\n" + juce::String(width, 6) + "\n" + juce::String(height, 6) + "\n"
              + juce::String(justification.getFlags()) + (ellipsis ? "e" : "");
    if (const auto found = layouts.find(name); found != layouts.end()) return found->second;
    if (layouts.size() >= 512) layouts.clear();
    juce::GlyphArrangement arrangement;
    arrangement.addCurtailedLineOfText(font, text, 0.0f, 0.0f, width, ellipsis);
    arrangement.justifyGlyphs(0, arrangement.getNumGlyphs(), 0.0f, 0.0f, width, height, justification);
    GlyphRecorder recorder;
    {
        juce::Graphics recording(recorder);
        arrangement.draw(recording);
    }
    std::vector<GlyphRun> runs;
    for (auto& recorded : recorder.runs) {
        GlyphRun run { recorded.font, {}, std::move(recorded.positions) };
        for (const auto glyph : recorded.glyphs) {
            juce::Path outline;
            recorded.font.getTypefacePtr()->getOutlineForGlyph(glyph, outline);
            run.outlines.push_back(std::move(outline));
        }
        runs.push_back(std::move(run));
    }
    return layouts[std::move(name)] = std::move(runs);
}
}
/** Graphics::drawText on one line: the same glyphs in the same places. With
    a gradient (`gradient`, as textFill returns) on CoreGraphics, each glyph's
    outline comes from TextCache and is filled with the very transform
    CoreGraphicsContext::drawGlyphs computes; the software renderer and
    Direct2D keep their own way. */
inline void drawTextLine(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                         juce::Justification justification, bool gradient, bool ellipsis = true) {
   #if JUCE_MAC
    if (gradient && TextCache::enabled
        && dynamic_cast<juce::LowLevelGraphicsSoftwareRenderer*>(&g.getInternalContext()) == nullptr) {
        if (text.isEmpty() || !g.clipRegionIntersects(area.getSmallestIntegerContainer())) return;
        const auto place = juce::AffineTransform::translation(area.getX(), area.getY());
        for (const auto& run : TextCache::glyphRuns(g.getCurrentFont(), text, area.getWidth(), area.getHeight(),
                                                    justification, ellipsis)) {
            const auto scale = run.font.getHeightInPoints();
            for (size_t i = 0; i < run.outlines.size(); ++i)
                if (!run.outlines[i].isEmpty())
                    g.fillPath(run.outlines[i], juce::AffineTransform::scale(scale * run.font.getHorizontalScale(), scale)
                                                    .translated(run.positions[i])
                                                    .followedBy(place));
        }
        return;
    }
   #endif
    juce::ignoreUnused(gradient);
    g.drawText(text, area, justification, ellipsis);
}
/** Figma-style corner radius: never more than half the shorter side. */
inline float cornerRadius(juce::Rectangle<float> area, float radius) {
    return juce::jmin(radius, juce::jmin(area.getWidth(), area.getHeight()) * 0.5f);
}
/** The brand's neutral box: the grey text gradient at 12%, no outline. Hover
    "lifts" it (brighter and a little more opaque) instead of tinting it. */
inline void fillBox(juce::Graphics& g, juce::Rectangle<float> area, float radius,
                    bool lit = false, float alpha = 1.0f) {
    if (area.isEmpty()) return;
    const auto top = lit ? textPrimary.brighter(0.18f) : textPrimary;
    const auto bottom = lit ? textSecondary.brighter(0.18f) : textSecondary;
    juce::ColourGradient gradient(top, 0.0f, area.getY(), bottom, 0.0f, area.getBottom(), false);
    gradient.multiplyOpacity(0.12f * (lit ? 1.18f : 1.0f) * alpha);
    g.setGradientFill(gradient);
    if (radius <= 0.0f) g.fillRect(area);
    else g.fillRoundedRectangle(area, cornerRadius(area, radius));
}
/** Gradient text on one line. Text that doesn't fit shrinks (down to 55%)
    rather than being squashed, which would make thin glyphs vanish. */
inline void drawText(juce::Graphics& g, const juce::String& text, juce::Font textFont,
                     juce::Rectangle<float> area, juce::Colour top, juce::Colour bottom,
                     juce::Justification justification = juce::Justification::centred, float alpha = 1.0f) {
    // Outside the area being painted nothing would be drawn: no measuring either.
    if (text.isEmpty() || !g.clipRegionIntersects(area.getSmallestIntegerContainer())) return;
    const auto width = TextCache::width(textFont, text);
    if (width > area.getWidth() && width > 0.0f)
        textFont = textFont.withHeight(textFont.getHeight() * juce::jmax(0.55f, area.getWidth() / width));
    const auto band = area.withSizeKeepingCentre(area.getWidth(), juce::jmin(area.getHeight(), textFont.getHeight()));
    g.setFont(textFont);
    const bool gradient = textFill(g, textFont, top.withMultipliedAlpha(alpha), bottom.withMultipliedAlpha(alpha),
                                   band.getY(), band.getBottom());
    drawTextLine(g, text, area, justification, gradient);
}
inline void drawGreyText(juce::Graphics& g, const juce::String& text, const juce::Font& textFont,
                         juce::Rectangle<float> area,
                         juce::Justification justification = juce::Justification::centred, float alpha = 1.0f) {
    drawText(g, text, textFont, area, textPrimary, textSecondary, justification, alpha);
}
inline void drawOceanText(juce::Graphics& g, const juce::String& text, const juce::Font& textFont,
                          juce::Rectangle<float> area,
                          juce::Justification justification = juce::Justification::centred, float alpha = 1.0f) {
    drawText(g, text, textFont, area, oceanHighlight, oceanTop, justification, alpha);
}
/** Selected list row (full) and hovered row (×0.21): accent from 15% on the
    left to 58% on the right, radius 6. */
inline void fillRowHighlight(juce::Graphics& g, juce::Rectangle<float> row, float strength = 1.0f) {
    juce::ColourGradient gradient(oceanHighlight.withAlpha(0.15f), row.getX(), row.getCentreY(),
                                  oceanHighlight.withAlpha(0.58f), row.getRight(), row.getCentreY(), false);
    gradient.multiplyOpacity(strength);
    g.setGradientFill(gradient);
    g.fillRoundedRectangle(row, cornerRadius(row, 6.0f));
}
/** What JUCE's ImageConvolutionKernel (9) with createGaussianBlur (2) does
    to `source` (each tap weighted exp(-(x²+y²)/8) over the kernel's sum, taps
    outside the image dropped, rounded once), done as two passes of 9 taps
    instead of one of 81: the weights are the products of two 9-tap rows. Only
    the order of the float sums differs (±1 level in an odd pixel). */
inline juce::Image gaussianBlur9(const juce::Image& source) {
    using namespace juce;
    if (! source.isValid()) return {};
    constexpr int taps = 9, half = taps / 2;
    float row[taps];
    double total = 0.0;
    for (int i = 0; i < taps; ++i) {
        row[i] = (float) std::exp(-1.0 / 8.0 * double((i - half) * (i - half)));
        total += row[i];
    }
    for (auto& w : row) w = float(double(w) / total);
    auto result = source.createCopy();
    const Image::BitmapData in(source, Image::BitmapData::readOnly);
    const Image::BitmapData out(result, Image::BitmapData::writeOnly);
    const int width = in.width, height = in.height, channels = in.pixelStride;
    std::vector<float> across((size_t) width * (size_t) height * (size_t) channels, 0.0f);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float* sum = &across[((size_t) y * (size_t) width + (size_t) x) * (size_t) channels];
            for (int t = 0; t < taps; ++t) {
                const int sx = x + t - half;
                if (sx < 0 || sx >= width) continue;
                const uint8* pixel = in.getPixelPointer(sx, y);
                for (int c = 0; c < channels; ++c) sum[c] += row[t] * pixel[c];
            }
        }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float sum[4] {};
            for (int t = 0; t < taps; ++t) {
                const int sy = y + t - half;
                if (sy < 0 || sy >= height) continue;
                const float* line = &across[((size_t) sy * (size_t) width + (size_t) x) * (size_t) channels];
                for (int c = 0; c < channels; ++c) sum[c] += row[t] * line[c];
            }
            uint8* pixel = out.getPixelPointer(x, y);
            for (int c = 0; c < channels; ++c) pixel[c] = (uint8) jmin(0xff, roundToInt(sum[c]));
        }
    return result;
}
/** Blurred backdrop for in-editor modals (Settings, effects, prompts): a 1/3
    copy of a snapshot with a 9-tap gaussian. Drawn stretched, under a 54% veil. */
inline juce::Image blurredBackdrop(const juce::Image& snapshot) {
    if (! snapshot.isValid()) return {};
    return gaussianBlur9(snapshot.rescaled(juce::jmax(1, snapshot.getWidth() / 3), juce::jmax(1, snapshot.getHeight() / 3)));
}
/** Card/side-panel/modal surface: the vertical panel gradient, no outline. */
inline void panel(juce::Graphics& g, juce::Rectangle<float> area, float radius = 9.0f) {
    g.setGradientFill(juce::ColourGradient(panelTop, area.getX(), area.getY(),
                                          panelBottom, area.getX(), area.getBottom(), false));
    if (radius <= 0.0f) g.fillRect(area);
    else g.fillRoundedRectangle(area, cornerRadius(area, radius));
}
}
