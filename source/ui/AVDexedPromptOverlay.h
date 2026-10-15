// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>

/** In-editor dialog for short questions and names (new folder, rename, import,
    confirmations, notices). It replaces juce::AlertWindow, which opens as its
    own desktop window: that one could be dragged around, ignored the editor zoom
    and sat off-centre. This one is a child of the editor's frame, so it cannot
    move, follows the zoom, and sits centred over a blurred photo of the editor
    (the Settings backdrop) with the 30 px help footer left uncovered.

    Geometry follows GOZO's dialog: 460 px panel, 48 px title band (SemiBold 20),
    body Regular 15, a 30 px text field in the 12% box, 118×32 pills on the right,
    a 15 px close X, an optional third pill left of cancel. Fades 83 ms in /
    117 ms out. Return confirms (unless the caller turns that off); Escape, the X
    or a click outside cancel; Tab cycles inside the dialog. A validator can keep
    it open with a message. */
class AVDexedPromptOverlay final : public juce::Component, private juce::ComponentListener {
public:
    struct Options {
        juce::String title, message;
        bool hasEditor = false;
        juce::String editorText, placeholder;
        int maxLength = 64;
        juce::String confirmText, cancelText;   // empty cancel = a single button
        /** Optional third button, left of cancel (e.g. "Descartar"): closes the
            dialog and calls onAlt instead of the result. */
        juce::String altText;
        std::function<void()> onAlt;
        /** Return confirms (default). Off where confirming would overwrite something. */
        bool returnConfirms = true;
        /** Returns an error to show (dialog stays open) or an empty string. */
        std::function<juce::String(const juce::String&)> validate;
    };
    using Result = std::function<void(bool confirmed, const juce::String& text)>;

    AVDexedPromptOverlay(juce::Component& frame, const juce::Image& editorSnapshot, Options, Result);
    ~AVDexedPromptOverlay() override;

    /** Adds itself hidden to the frame, fades in and takes keyboard focus. */
    void present();
    /** Closes the dialog from outside, with the normal fade-out: what it was
        waiting for has happened (a MIDI learn received its CC). The result is
        still delivered, as a cancel (confirmed = false), so the owner's
        bookkeeping runs exactly once. Does nothing once the dialog is closing. */
    void dismiss();
    bool isFinishing() const noexcept { return AVFinished; }

    void paint(juce::Graphics&) override;
    void resized() override;
    bool hitTest(int x, int y) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void inputAttemptWhenModal() override;

private:
    juce::Component::SafePointer<juce::Component> AVFrame;
    Options AVOptions;
    Result AVResult;
    juce::Image AVBackdrop;
    std::unique_ptr<juce::TextEditor> AVEditor;
    juce::TextButton AVConfirm, AVCancel, AVAlt;
    std::unique_ptr<juce::Drawable> AVCloseIcon, AVCloseIconHover;
    juce::String AVError;
    juce::Rectangle<int> AVPanel;
    juce::StringArray AVMessageLines, AVErrorLines;
    bool AVCloseHovered = false, AVFinished = false;

    juce::Rectangle<int> closeBounds() const;
    void layoutPanel();
    /** `alternative`: the third pill was chosen (onAlt runs instead of the result). */
    void finish(bool confirmed, bool alternative = false);
    void componentMovedOrResized(juce::Component&, bool, bool) override;
};
