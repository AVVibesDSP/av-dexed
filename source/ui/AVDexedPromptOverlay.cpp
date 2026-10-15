// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedPromptOverlay.h"
#include "AVDexedTheme.h"
#include "AVDexedLanguage.h"
#include "../JuceLibraryCode/BinaryData.h"

using namespace juce;

namespace {
// GOZO dialog geometry (logical px).
constexpr int panelW = 460, titleH = 48, padX = 28, lineH = 21, editorH = 30, buttonH = 32, pillW = 118, pillGap = 12;
constexpr int footerH = 30, closeSide = 15;
const Colour errorColour { 0xffff6b6b };

std::unique_ptr<Drawable> closeIcon(bool hover) {
    auto text = String::fromUTF8(BinaryData::AVDexed_CloseIcon_svg, BinaryData::AVDexed_CloseIcon_svgSize);
    if (hover)
        text = text.replace("#E0E0E0", "#" + AVDexedTheme::oceanHighlight.toDisplayString(false), true)
                   .replace("#9A9A9A", "#" + AVDexedTheme::oceanTop.toDisplayString(false), true);
    if (auto xml = XmlDocument::parse(text)) return Drawable::createFromSVG(*xml);
    return {};
}

constexpr int errorLineH = 18;

/** Word-wraps `text` to `width` with `font`; keeps explicit newlines. */
StringArray wrapLines(const String& text, float width, const Font& font) {
    StringArray lines;
    if (text.isEmpty()) return lines;
    for (const auto& paragraph : StringArray::fromLines(text)) {
        String line;
        for (auto word : StringArray::fromTokens(paragraph, " ", {})) {
            // A word wider than a line (a long file path in an error) is cut
            // where it stops fitting, instead of being squeezed or ellipsised.
            while (word.length() > 1 && GlyphArrangement::getStringWidth(font, word) > width) {
                int fits = word.length() - 1;
                while (fits > 1 && GlyphArrangement::getStringWidth(font, word.substring(0, fits)) > width) --fits;
                if (line.isNotEmpty()) { lines.add(line); line.clear(); }
                lines.add(word.substring(0, fits));
                word = word.substring(fits);
            }
            const auto candidate = line.isEmpty() ? word : line + " " + word;
            if (line.isNotEmpty() && GlyphArrangement::getStringWidth(font, candidate) > width) {
                lines.add(line);
                line = word;
            } else {
                line = candidate;
            }
        }
        lines.add(line);
    }
    return lines;
}
}

AVDexedPromptOverlay::AVDexedPromptOverlay(Component& frame, const Image& editorSnapshot, Options options, Result result)
    : AVFrame(&frame), AVOptions(std::move(options)), AVResult(std::move(result)),
      AVBackdrop(AVDexedTheme::blurredBackdrop(editorSnapshot)) {
    auto& language = AVDexedLanguageService::getInstance();
    if (AVOptions.confirmText.isEmpty()) AVOptions.confirmText = language.text("ok");
    AVCloseIcon = closeIcon(false);
    AVCloseIconHover = closeIcon(true);

    if (AVOptions.hasEditor) {
        AVEditor = std::make_unique<TextEditor>();
        AVEditor->setFont(AVDexedTheme::font(15.0f, "Medium"));
        AVEditor->setJustification(Justification::centredLeft);
        AVEditor->setIndents(10, 0);
        AVEditor->setMultiLine(false);
        AVEditor->setReturnKeyStartsNewLine(false);
        AVEditor->setScrollbarsShown(false);
        AVEditor->setPopupMenuEnabled(false);
        AVEditor->setSelectAllWhenFocused(true);
        AVEditor->setInputRestrictions(jmax(1, AVOptions.maxLength));
        AVEditor->setColour(TextEditor::backgroundColourId, Colours::transparentBlack);
        AVEditor->setColour(TextEditor::outlineColourId, Colours::transparentBlack);
        AVEditor->setColour(TextEditor::focusedOutlineColourId, Colours::transparentBlack);
        AVEditor->setColour(TextEditor::textColourId, AVDexedTheme::textPrimary);
        AVEditor->setColour(TextEditor::highlightColourId, AVDexedTheme::oceanTop.withAlpha(0.35f));
        AVEditor->setColour(TextEditor::highlightedTextColourId, AVDexedTheme::textPrimary);
        AVEditor->setColour(CaretComponent::caretColourId, AVDexedTheme::oceanHighlight);
        AVEditor->setTextToShowWhenEmpty(AVOptions.placeholder, AVDexedTheme::textSecondary.withAlpha(0.6f));
        AVEditor->setText(AVOptions.editorText, false);
        AVEditor->setTitle(AVOptions.title);
        AVEditor->onReturnKey = [this] { if (AVOptions.returnConfirms) finish(true); };
        AVEditor->onEscapeKey = [this] { finish(false); };
        AVEditor->onTextChange = [this] {
            if (AVError.isNotEmpty()) { AVError.clear(); layoutPanel(); repaint(); }
        };
        addAndMakeVisible(*AVEditor);
    }
    AVConfirm.setButtonText(AVOptions.confirmText);
    AVConfirm.setWantsKeyboardFocus(true);
    AVConfirm.onClick = [this] { finish(true); };
    addAndMakeVisible(AVConfirm);
    if (AVOptions.cancelText.isNotEmpty()) {
        AVCancel.setButtonText(AVOptions.cancelText);
        AVCancel.setWantsKeyboardFocus(true);
        AVCancel.onClick = [this] { finish(false); };
        addAndMakeVisible(AVCancel);
    }
    if (AVOptions.altText.isNotEmpty()) {
        AVAlt.setButtonText(AVOptions.altText);
        AVAlt.setWantsKeyboardFocus(true);
        AVAlt.onClick = [this] { finish(false, true); };
        addAndMakeVisible(AVAlt);
    }
    setTitle(AVOptions.title);
    setWantsKeyboardFocus(true);
    // Tab cycles inside the dialog: leaving it would reach a control blocked by
    // the modal state, and JUCE reports that as a click outside (= cancel).
    setFocusContainerType(FocusContainerType::keyboardFocusContainer);
    frame.addComponentListener(this);
}

AVDexedPromptOverlay::~AVDexedPromptOverlay() {
    // Only a fade-in still running is stopped. The fade-out animates a proxy
    // image the animator owns and deletes when it ends; cancelling it here
    // would make every closed dialog vanish at once instead of fading. The
    // editor cancels the fades left when it closes itself.
    if (! AVFinished) Desktop::getInstance().getAnimator().cancelAnimation(this, false);
    if (AVFrame != nullptr) AVFrame->removeComponentListener(this);
}

void AVDexedPromptOverlay::present() {
    if (AVFrame == nullptr) return;
    // Added hidden and faded in BEFORE enterModalState(), which makes it
    // visible: ComponentAnimator::fadeIn() does nothing to an already visible,
    // opaque component.
    AVFrame->addChildComponent(*this);
    setBounds(AVFrame->getLocalBounds());
    toFront(false);
    Desktop::getInstance().getAnimator().fadeIn(this, 83);
    enterModalState(true);
    Component::SafePointer<AVDexedPromptOverlay> safeThis(this);
    MessageManager::callAsync([safeThis] {
        if (safeThis == nullptr || safeThis->AVFinished) return;
        if (safeThis->AVEditor != nullptr) {
            safeThis->AVEditor->grabKeyboardFocus();
            safeThis->AVEditor->selectAll();
        } else if (! safeThis->AVOptions.returnConfirms) {
            // A focused pill would take Return as a click: the dialog itself
            // holds the focus, so Return does nothing until a pill is chosen.
            safeThis->grabKeyboardFocus();
        } else {
            safeThis->AVConfirm.grabKeyboardFocus();
        }
    });
}

Rectangle<int> AVDexedPromptOverlay::closeBounds() const {
    return { AVPanel.getRight() - 24 - closeSide, AVPanel.getY() + (titleH - closeSide) / 2, closeSide, closeSide };
}

void AVDexedPromptOverlay::layoutPanel() {
    AVMessageLines = wrapLines(AVOptions.message, float(panelW - 2 * padX), AVDexedTheme::font(15.0f));
    // A validation error wraps like the message (a long one, e.g. a read-only
    // folder in Spanish, would otherwise lose its end to an ellipsis).
    AVErrorLines = wrapLines(AVError, float(panelW - 2 * padX - 2), AVDexedTheme::font(13.0f));
    int height = titleH + 24 + AVMessageLines.size() * lineH;
    if (AVEditor != nullptr) height += (AVMessageLines.isEmpty() ? 0 : 12) + editorH + 18;
    if (! AVErrorLines.isEmpty()) height += 4 + AVErrorLines.size() * errorLineH;
    if (AVEditor == nullptr && AVMessageLines.isEmpty()) height += lineH;
    height += 4 + buttonH + 24;
    AVPanel = Rectangle<int>(panelW, height).withCentre(getLocalBounds().withTrimmedBottom(footerH).getCentre());

    int y = AVPanel.getY() + titleH + 24 + AVMessageLines.size() * lineH;
    if (AVEditor != nullptr) {
        if (! AVMessageLines.isEmpty()) y += 12;
        AVEditor->setBounds(AVPanel.getX() + padX, y, panelW - 2 * padX, editorH);
    }
    // Pills right to left, as in GOZO: confirm, cancel, then the third one.
    const int buttonsY = AVPanel.getBottom() - 24 - buttonH;
    const auto pillWidth = [](const String& text) {
        return jmax(pillW, int(GlyphArrangement::getStringWidth(AVDexedTheme::font(15.0f), text)) + 32);
    };
    const int confirmW = pillWidth(AVOptions.confirmText);
    AVConfirm.setBounds(AVPanel.getRight() - padX - confirmW, buttonsY, confirmW, buttonH);
    int left = AVConfirm.getX();
    if (AVOptions.cancelText.isNotEmpty()) {
        const int cancelW = pillWidth(AVOptions.cancelText);
        AVCancel.setBounds(left - pillGap - cancelW, buttonsY, cancelW, buttonH);
        left = AVCancel.getX();
    }
    if (AVOptions.altText.isNotEmpty()) {
        const int altW = pillWidth(AVOptions.altText);
        AVAlt.setBounds(left - pillGap - altW, buttonsY, altW, buttonH);
    }
}

void AVDexedPromptOverlay::resized() { layoutPanel(); }

void AVDexedPromptOverlay::paint(Graphics& g) {
    {
        Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(0, 0, getWidth(), getHeight() - footerH);   // the help footer stays visible
        if (AVBackdrop.isValid()) g.drawImage(AVBackdrop, getLocalBounds().toFloat());
        g.setColour(Colours::black.withAlpha(0.54f));
        g.fillAll();
    }
    const auto panel = AVPanel.toFloat();
    DropShadow(Colours::black.withAlpha(0.6f), 36, { 0, 8 }).drawForRectangle(g, AVPanel);
    AVDexedTheme::panel(g, panel, 12.0f);
    // Title band: the panel gradient over its own height, rounded on top only.
    Path band;
    band.addRoundedRectangle(panel.getX(), panel.getY(), panel.getWidth(), float(titleH), 12.0f, 12.0f, true, true, false, false);
    g.setGradientFill(ColourGradient(AVDexedTheme::panelTop, 0.0f, panel.getY(), AVDexedTheme::panelBottom, 0.0f, panel.getY() + titleH, false));
    g.fillPath(band);
    AVDexedTheme::drawGreyText(g, AVOptions.title, AVDexedTheme::font(20.0f, "SemiBold"),
                               panel.withHeight(float(titleH)).reduced(52.0f, 0.0f), Justification::centred);
    if (auto* icon = AVCloseHovered ? AVCloseIconHover.get() : AVCloseIcon.get())
        icon->drawWithin(g, closeBounds().toFloat(), RectanglePlacement::centred, 1.0f);

    float y = panel.getY() + titleH + 24.0f;
    for (const auto& line : AVMessageLines) {
        AVDexedTheme::drawGreyText(g, line, AVDexedTheme::font(15.0f), { panel.getX() + padX, y, panel.getWidth() - 2 * padX, float(lineH) },
                                   Justification::centredLeft, 0.92f);
        y += lineH;
    }
    if (AVEditor != nullptr) {
        const auto box = AVEditor->getBounds().toFloat();
        AVDexedTheme::fillBox(g, box, 6.0f, AVEditor->hasKeyboardFocus(true));
        g.setColour(errorColour);
        g.setFont(AVDexedTheme::font(13.0f));
        float errorY = box.getBottom() + 6.0f;
        for (const auto& line : AVErrorLines) {
            g.drawText(line, Rectangle<float>(box.getX() + 2.0f, errorY, box.getWidth() - 2.0f, float(errorLineH)),
                       Justification::centredLeft, false);
            errorY += float(errorLineH);
        }
    }
}

bool AVDexedPromptOverlay::hitTest(int, int y) { return y < getHeight() - footerH; }

void AVDexedPromptOverlay::mouseDown(const MouseEvent& event) {
    if (closeBounds().expanded(8).contains(event.getPosition()) || ! AVPanel.contains(event.getPosition()))
        finish(false);
}

void AVDexedPromptOverlay::mouseMove(const MouseEvent& event) {
    const bool hovered = closeBounds().expanded(8).contains(event.getPosition());
    if (hovered != AVCloseHovered) { AVCloseHovered = hovered; repaint(closeBounds().expanded(4)); }
}

void AVDexedPromptOverlay::mouseExit(const MouseEvent&) {
    if (AVCloseHovered) { AVCloseHovered = false; repaint(closeBounds().expanded(4)); }
}

bool AVDexedPromptOverlay::keyPressed(const KeyPress& key) {
    AVDexedTheme::noteKeyboardNavigation(key);
    if (key == KeyPress::escapeKey) { finish(false); return true; }
    if (key == KeyPress::returnKey) {
        if (AVOptions.returnConfirms) finish(true);
        return true;   // swallowed either way: it must not reach the editor
    }
    // The dialog itself holds the focus when Return must not confirm. Its own
    // Tab would look for the next control outside it (and cancel): enter the
    // pills instead, from the left with Tab, from the right with Shift+Tab.
    if (key.isKeyCode(KeyPress::tabKey) && hasKeyboardFocus(false)) {
        if (auto traverser = createKeyboardFocusTraverser()) {
            const auto pills = traverser->getAllComponents(this);
            if (! pills.empty())
                (key.getModifiers().isShiftDown() ? pills.back() : pills.front())->grabKeyboardFocus();
        }
        return true;
    }
    return false;
}

void AVDexedPromptOverlay::inputAttemptWhenModal() { finish(false); }

void AVDexedPromptOverlay::dismiss() { finish(false); }

void AVDexedPromptOverlay::finish(bool confirmed, bool alternative) {
    if (AVFinished) return;
    const auto text = AVEditor != nullptr ? AVEditor->getText().trim() : String();
    if (confirmed && ! alternative && AVOptions.validate) {
        const auto error = AVOptions.validate(text);
        if (error.isNotEmpty()) {
            AVError = error;
            layoutPanel();
            repaint();
            if (AVEditor != nullptr) AVEditor->grabKeyboardFocus();
            return;
        }
    }
    AVFinished = true;
    exitModalState(0);
    auto& animator = Desktop::getInstance().getAnimator();
    animator.cancelAnimation(this, false);
    animator.fadeOut(this, 117);   // GOZO dialog timing: in ~5 frames, out ~7 frames
    // Report after the click/key handler has unwound: the result may open
    // another prompt or delete this one.
    auto result = AVResult;
    auto onAlt = AVOptions.onAlt;
    MessageManager::callAsync([result, onAlt, alternative, confirmed, text] {
        if (alternative) { if (onAlt) onAlt(); return; }
        if (result) result(confirmed, text);
    });
}

void AVDexedPromptOverlay::componentMovedOrResized(Component& component, bool, bool wasResized) {
    if (wasResized && &component == AVFrame.getComponent()) setBounds(AVFrame->getLocalBounds());
}
