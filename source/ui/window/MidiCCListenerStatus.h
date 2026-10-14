#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "midi/MidiCCHandler.h"
#include "../../debugger/Logger.h"
#include "../AVDexedLanguage.h"
#include "../DXLookNFeel.h"

class MidiCCListenerStatus: public AVAlertWindow, private juce::Timer {
    MidiCCHandler *handler;
    juce::Component::SafePointer<juce::Component> AVParent;
public :
    MidiCCListenerStatus(juce::Component *parent, MidiCCHandler *handler, juce::RangedAudioParameter *parameter) :
            AVAlertWindow(AVDexedLanguageService::getInstance().text("midiLearnTitle"), "", AlertWindow::InfoIcon, parent), handler(handler), AVParent(parent) {
        auto& language = AVDexedLanguageService::getInstance();
        setMessage(language.format("midiLearnMessage", {{ "parameter", parent->getTitle() }}));
        addButton(language.text("cancel"), -1, juce::KeyPress(juce::KeyPress::escapeKey));
        const auto safe = juce::Component::SafePointer<MidiCCListenerStatus>(this);
        handler->startMapping(parameter, [safe] {
            if (safe != nullptr) safe->exitModalState(1);
        });
        enterModalState(true, nullptr, true);
        startTimer(100);
    }

    ~MidiCCListenerStatus() {
        stopTimer();
        if (AVParent != nullptr) handler->cancelMapping();
    }
private:
    void timerCallback() override {
        if (AVParent == nullptr) { stopTimer(); exitModalState(-1); }
    }
};
