#include "PluginProcessor.h"
#include "parameter/Model.h"
#include "ui/AVDexedLanguage.h"
#include "ui/DXLookNFeel.h"

int DexedAudioProcessor::tuningTranspositionShift() {
    int transpose = activeProgram[IDs::transpose.pos];
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
}

void DexedAudioProcessor::retuneToStandard() {
    currentSCLData = "";
    currentKBMData = "";
    resetTuning(createStandardTuning());
}

namespace {
void AVShowTuningFileError(AudioProcessorEditor* editor, const String& message) {
    auto& language = AVDexedLanguageService::getInstance();
    auto* alert = new AVAlertWindow(language.text("tuningFileError"), message, AlertWindow::WarningIcon, editor);
    alert->addButton(language.text("ok"), 0, KeyPress(KeyPress::returnKey));
    alert->enterModalState(true, nullptr, true);
}

void AVShowTuningParseError(AudioProcessor& processor, const std::string& detail) {
    const auto message = AVDexedLanguageService::getInstance().format("tuningReadError", {{"detail", String(detail)}});
    if (MessageManager::existsAndIsCurrentThread()) {
        AVShowTuningFileError(processor.getActiveEditor(), message);
        return;
    }
    // A host may restore state on its own thread: build no window there and
    // hold no editor pointer; show the notice later, on the message thread.
    MessageManager::callAsync([message] { AVShowTuningFileError(nullptr, message); });
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
