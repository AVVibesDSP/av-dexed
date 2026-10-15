#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "midi/MidiCCHandler.h"
#include "../PluginEditor.h"

/** "Asignar CC MIDI" from a control's right-click menu: the editor's own
    waiting dialog (DexedAudioProcessorEditor::showAVMidiLearn: "Aprender MIDI",
    blurred, fixed, a single Cancel), no longer a desktop AlertWindow. The menus
    create this with `new`, as they did the old window. The dialog and the
    Learn keep their own state, so it only starts them and then releases
    itself. A control outside an editor has nowhere to show the dialog and
    learns nothing. The editor's processor owns `handler`, the one it uses. */
class MidiCCListenerStatus final {
public:
    MidiCCListenerStatus(juce::Component* control, MidiCCHandler*, juce::RangedAudioParameter* parameter) {
        if (control != nullptr && parameter != nullptr)
            if (auto* editor = control->findParentComponentOfClass<DexedAudioProcessorEditor>())
                editor->showAVMidiLearn(*control, *parameter);
        juce::MessageManager::callAsync([this] { delete this; });
    }
};
