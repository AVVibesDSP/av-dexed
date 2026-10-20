#include "PluginProcessor.h"
#include "parameter/Model.h"
#include "ui/AVDexedLanguage.h"
#include "ui/DXLookNFeel.h"
#include "ui/PluginEditor.h"

int DexedAudioProcessor::tuningTranspositionShift() {
    return tuningTranspositionShift(activeProgram[IDs::transpose.pos]);
}

int DexedAudioProcessor::tuningTranspositionShift(int transpose) {
    if( synthTuningState->is_standard_tuning() || ! controllers.transpose12AsScale )
        return transpose - 24;
    else {
        int d144 = transpose;
        if( d144 % 12 == 0 ) {
            int oct = (d144 - 24) / 12;
            int res = oct * synthTuningState->scale_length();
            return res;
        }
        else
            return transpose - 24;
    }
}

void DexedAudioProcessor::resetTuning(std::shared_ptr<TuningState> t) {
    synthTuningState = t;
    synthTuningStateLast = t;
    for( int i=0; i<MAX_ACTIVE_NOTES; ++i )
        if( voices[i].dx7_note != nullptr )
            voices[i].dx7_note->tuning_state_ = synthTuningState;
    for (auto& layer : avLayers)
        layer.setTuning(synthTuningState);
}

void DexedAudioProcessor::retuneToStandard() {
    currentSCLData = "";
    currentKBMData = "";
    resetTuning(createStandardTuning());
}

namespace {
/** A problem with a .scl/.kbm (SCL/KBM buttons, a profile, a restored state)
    is an in-editor notice, as for a dropped tuning file; AVShowMessage falls
    back to an AV alert window only when no editor is open. */
void AVShowTuningFileError(AudioProcessorEditor* editor, const String& message) {
    AVShowMessage(AlertWindow::WarningIcon, AVDexedLanguageService::getInstance().text("tuningFileError"), message, editor);
}

/** The open editor of `processor`, found by address only: by the time an
    asynchronous notice runs, the processor may be gone, so it is never touched. */
DexedAudioProcessorEditor* AVOpenEditorOf(const AudioProcessor* processor) {
    std::function<DexedAudioProcessorEditor*(Component&)> search = [&](Component& component) -> DexedAudioProcessorEditor* {
        if (auto* editor = dynamic_cast<DexedAudioProcessorEditor*>(&component))
            return editor->getAudioProcessor() == processor ? editor : nullptr;
        for (auto* child : component.getChildren())
            if (auto* found = search(*child)) return found;
        return nullptr;
    };
    // The editor sits inside a desktop window: the host's wrapper or the
    // standalone app's window.
    auto& desktop = Desktop::getInstance();
    for (int index = 0; index < desktop.getNumComponents(); ++index)
        if (auto* top = desktop.getComponent(index))
            if (auto* found = search(*top)) return found;
    return nullptr;
}

void AVShowTuningParseError(AudioProcessor& processor, const std::string& detail) {
    const auto message = AVDexedLanguageService::getInstance().format("tuningReadError", {{"detail", String(detail)}});
    if (MessageManager::existsAndIsCurrentThread()) {
        AVShowTuningFileError(processor.getActiveEditor(), message);
        return;
    }
    // A host may restore state on its own thread: build no window there and
    // hold no editor pointer; on the message thread, look for this
    // processor's editor (if one is open) and show the notice there.
    const AudioProcessor* owner = &processor;
    MessageManager::callAsync([message, owner] { AVShowTuningFileError(AVOpenEditorOf(owner), message); });
}
}

void DexedAudioProcessor::applySCLTuning() {
    tuningFileChooser = std::make_unique<FileChooser>(AVDexedLanguageService::getInstance().text("loadScaleTitle"), File(), "*.scl");
    tuningFileChooser->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
        [this](const FileChooser& chooser) {
            if (chooser.getResults().isEmpty()) return;
            const auto file = chooser.getResult();
            auto& language = AVDexedLanguageService::getInstance();
            if (!file.hasFileExtension(".scl")) {
                AVShowTuningFileError(getActiveEditor(), language.format("tuningFileExtension", {{"extension", ".scl"}}));
                return;
            }
            if (file.getSize() > MAX_SCL_KBM_FILE_SIZE) {
                AVShowTuningFileError(getActiveEditor(), language.format("tuningFileSize", {{"bytes", String(MAX_SCL_KBM_FILE_SIZE)}}));
                return;
            }
            if (file.getSize() <= 0) {
                AVShowTuningFileError(getActiveEditor(), language.text("tuningFileEmpty"));
                return;
            }
            applySCLTuning(file);
        });
}

void DexedAudioProcessor::applySCLTuning(File s) {
    std::string sclcontents = s.loadFileAsString().toStdString();
    applySCLTuning(sclcontents);
}

void DexedAudioProcessor::applySCLTuning(std::string sclcontents) {
    if( currentKBMData.size() < 1 ) {
        std::string error;
        auto t = createTuningFromSCLData( sclcontents, &error );
        if (t != nullptr) {
            resetTuning(t); // update tuning
            currentSCLData = sclcontents; // remember this SCL data
            synthTuningStateLast = t; // remember this whole state as a "last good working state"
        }
        else {
            resetTuning(synthTuningStateLast); // revert to the "last good working state"
            AVShowTuningParseError(*this, error);
        }
    }
    else {
        std::string error;
        auto t = createTuningFromSCLAndKBMData( sclcontents, currentKBMData, &error );
        if (t != nullptr) {
            resetTuning(t); // update tuning
            currentSCLData = sclcontents; // remember this SCL data
            synthTuningStateLast = t; // remember this whole state as a "last good working state"
        }
        else {
            resetTuning(synthTuningStateLast); // revert to the "last good working state"
            AVShowTuningParseError(*this, error);
        }
    }
}

void DexedAudioProcessor::applyKBMMapping() {
    tuningFileChooser = std::make_unique<FileChooser>(AVDexedLanguageService::getInstance().text("loadMapTitle"), File(), "*.kbm");
    tuningFileChooser->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
        [this](const FileChooser& chooser) {
            if (chooser.getResults().isEmpty()) return;
            const auto file = chooser.getResult();
            auto& language = AVDexedLanguageService::getInstance();
            if (!file.hasFileExtension(".kbm")) {
                AVShowTuningFileError(getActiveEditor(), language.format("tuningFileExtension", {{"extension", ".kbm"}}));
                return;
            }
            if (file.getSize() > MAX_SCL_KBM_FILE_SIZE) {
                AVShowTuningFileError(getActiveEditor(), language.format("tuningFileSize", {{"bytes", String(MAX_SCL_KBM_FILE_SIZE)}}));
                return;
            }
            if (file.getSize() <= 0) {
                AVShowTuningFileError(getActiveEditor(), language.text("tuningFileEmpty"));
                return;
            }
            applyKBMMapping(file);
        });
}

void DexedAudioProcessor::applyKBMMapping( File s ) {
    std::string kbmcontents = s.loadFileAsString().toStdString();
    applyKBMMapping(kbmcontents);
}

void DexedAudioProcessor::applyKBMMapping(std::string kbmcontents) {
    if( currentSCLData.size() < 1 ) {
        std::string error;
        auto t = createTuningFromKBMData(kbmcontents, &error);
        if (t != nullptr) {
            resetTuning(t); // update tuning
            currentKBMData = kbmcontents; // remember this KBM data
            synthTuningStateLast = t; // remember this whole state as a "last good working state"
        } else {
            resetTuning(synthTuningStateLast); // revert to the "last good working state"
            AVShowTuningParseError(*this, error);
        }
    } else {
        std::string error;
        auto t = createTuningFromSCLAndKBMData( currentSCLData, kbmcontents, &error );
        if (t != nullptr) {
            resetTuning(t); // update tuning
            currentKBMData = kbmcontents; // remember this KBM data
            synthTuningStateLast = t; // remember this whole state as a "last good working state"
        } else {
            resetTuning(synthTuningStateLast); // revert to "last good working state"
            AVShowTuningParseError(*this, error);
        }
    }
}
