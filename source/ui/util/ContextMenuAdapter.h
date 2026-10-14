#pragma once

#include "ui/PluginEditor.h"
#include "midi/MidiCCHandler.h"
#include "../window/MidiCCListenerStatus.h"
#include "../AVDexedLanguage.h"

class ContextMenuAdapter : public juce::MouseListener {
protected:
    DexedAudioProcessor &processor;
public:
    ContextMenuAdapter(DexedAudioProcessor &processor) : processor(processor) {
    }

    virtual void addSpecificItems(PopupMenu &menu, juce::Component *source) {
    }

    void mouseDown(const MouseEvent &event) override {
        if ( !event.mods.isPopupMenu())
            return;

        PopupMenu popup;
        juce::Component *source = event.eventComponent;
        const auto safeSource = juce::Component::SafePointer<juce::Component>(source);
        auto& language = AVDexedLanguageService::getInstance();
        auto* processorPtr = &processor;
        juce::RangedAudioParameter *parameter = processor.parameters.getParameter(source->getName());

        if ( parameter != nullptr ) {
            int idx = processor.midiCCMapper->getMapping(source->getName());
            if ( idx != -1 ) {
                popup.addItem(language.format("removeMidiMapping", {{ "parameter", source->getTitle() }, { "cc", juce::String(idx) }}), [safeSource, processorPtr, idx] {
                    if (safeSource != nullptr) processorPtr->midiCCMapper->removeMapping(idx);
                });
            } else {
                popup.addItem(language.format("assignMidiMapping", {{ "parameter", source->getTitle() }}), [safeSource, processorPtr, parameter] {
                    if (safeSource != nullptr) new MidiCCListenerStatus(safeSource.getComponent(), processorPtr->midiCCMapper.get(), parameter);
                });
            }
            popup.addSeparator();
        }

        addSpecificItems(popup, source);
        popup.addItem(language.text("sendCurrentVoice"), [safeSource, processorPtr] {
            if (safeSource != nullptr) processorPtr->sendCurrentSysexProgram();
        });

        if ( processor.getZoomFactor() > 1.0f ) {
            popup.addSeparator();
            popup.addItem(language.text("resetUiScale"), [safeSource] {
                if (safeSource == nullptr) return;
                if (auto *editor = safeSource->findParentComponentOfClass<DexedAudioProcessorEditor>())
                    editor->resetZoomFactor();
            });
        }

        // A PopupMenu ignores its target's LookAndFeel; without this it is JUCE's default.
        popup.setLookAndFeel(&source->getLookAndFeel());
        auto options = juce::PopupMenu::Options().withTargetComponent(source).withMousePosition();
        popup.showMenuAsync(options);
    }
};
