#pragma once

#include "parameter/DexedApvts.h"
#include "../AVDexedLanguage.h"
#include "../AVDexedTheme.h"

/** Shows the localized label and value of the parameter under the pointer.
    Automation identifiers and parameter/preset data are never rewritten. */
class ParameterObserver : public juce::Component,
                          public juce::SettableTooltipClient,
                          private juce::ValueTree::Listener,
                          private juce::ChangeListener,
                          private juce::AsyncUpdater {
    const juce::String AVDefaultMessage { "AV Dexed " DEXED_VERSION };
    DexedApvts& apvts;
    juce::WeakReference<juce::Component> AVTopComponent;
    juce::RangedAudioParameter* parameter = nullptr;
public:
    ParameterObserver(juce::Component* topComponent, DexedApvts& state)
        : apvts(state), AVTopComponent(topComponent) {
        if (topComponent != nullptr) topComponent->addMouseListener(this, true);
        apvts.state.addListener(this);
        AVDexedLanguageService::getInstance().addChangeListener(this);
        AVRefreshAccessibleText();
    }

    ~ParameterObserver() override {
        cancelPendingUpdate();
        AVDexedLanguageService::getInstance().removeChangeListener(this);
        apvts.state.removeListener(this);
        if (auto* top = AVTopComponent.get()) top->removeMouseListener(this);
    }

    void mouseEnter(const juce::MouseEvent& event) override {
        // The numeric value's Label is a child of its Slider. Resolve through
        // parents so hovering that value reports the same real parameter.
        for (auto* component = event.eventComponent; component != nullptr; component = component->getParentComponent()) {
            if (auto* candidate = apvts.getParameter(component->getName())) {
                parameter = candidate;
                AVRefreshAccessibleText();
                repaint();
                return;
            }
            if (component == AVTopComponent.get()) break;
        }
    }

    void paint(juce::Graphics& g) override {
        const auto bounds = getLocalBounds().reduced(2, 0);
        const auto font = AVDexedTheme::font(10.5f, "Medium");
        g.setFont(font);
        const auto draw = [&](const juce::String& text, juce::Rectangle<int> area) {
            const float inkHeight = juce::jmin(float(area.getHeight()), font.getHeight());
            const float top = float(area.getCentreY()) - inkHeight * 0.5f;
            AVDexedTheme::textFill(g, font, AVDexedTheme::textPrimary, AVDexedTheme::textSecondary, top, top + inkHeight);
            g.drawText(text, area, juce::Justification::centred, true);
        };
        // The product line and its build id split like a long parameter value
        // instead of ending in an ellipsis.
        const auto name = parameter != nullptr ? AVParameterName() : juce::String("AV Dexed");
        const auto value = parameter != nullptr ? parameter->getCurrentValueAsText() : juce::String(DEXED_VERSION);
        const auto combined = parameter != nullptr ? name + ": " + value : AVDefaultMessage;
        if (juce::GlyphArrangement::getStringWidth(font, combined) <= float(bounds.getWidth()) || bounds.getHeight() < 24) {
            draw(combined, bounds);
        } else {
            // Preserve the readable value on its own line; the full localized
            // name is also available through the tooltip/accessibility title.
            const int firstHeight = bounds.getHeight() / 2;
            draw(name, bounds.withHeight(firstHeight));
            draw(value, bounds.withTrimmedTop(firstHeight));
        }
    }
private:
    juce::String AVParameterName() const {
        return parameter != nullptr
            ? AVDexedLanguageService::getInstance().parameterName(parameter->getParameterID(), parameter->getName(100))
            : AVDefaultMessage;
    }
    void AVRefreshAccessibleText() {
        const auto text = parameter != nullptr ? AVParameterName() + ": " + parameter->getCurrentValueAsText() : AVDefaultMessage;
        setTitle(text);
        setTooltip(text);
    }
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override {
        if (!apvts.pushToParameterInProgress) triggerAsyncUpdate();
    }
    void changeListenerCallback(juce::ChangeBroadcaster*) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override {
        AVRefreshAccessibleText();
        repaint();
    }
};
