#pragma once

#include "msfa/controllers.h"
#include "midi/SysexComm.h"
#include "parameter/DexedApvts.h"
#include "../util/AudioComponentContainer.h"
#include <functional>
#include "../DXLookNFeel.h"
#include "../AVDexedSettingsStyle.h"
#include "../AVDexedLanguage.h"

class DexedAudioProcessor;

class ParamDialog  : public Component,
                     public juce::Slider::Listener,
                     public juce::ComboBox::Listener,
                     public juce::Button::Listener,
                     public juce::Timer,
                     private juce::ChangeListener
{
public:
    //==============================================================================
    ParamDialog (DexedApvts &apvts, DexedAudioProcessor &proc);
    ~ParamDialog() override;

    void setDialogValues(Controllers &c, SysexComm &mgr, bool showKeyboard, float dpiScaleFactor);
    bool getDialogValues(Controllers &c, SysexComm &mgr, bool *showKeyboard, float *dpiScaleFactor);

    typedef enum {
        LOAD_SCL,
        LOAD_KBM,
        RESET_TUNING,
        SHOW_TUNING
    } TuningAction;

    void setTuningCallback(std::function<void(ParamDialog *, ParamDialog::TuningAction)> tc ) { tuning_callback_ = tc; }
    void setGeneralCallback(std::function<void(ParamDialog *)> gc ) { general_callback_ = gc; }

    void setIsStandardTuning(bool s);
    void timerCallback() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void sliderValueChanged (juce::Slider* sliderThatWasMoved) override;
    void comboBoxChanged (juce::ComboBox* comboBoxThatHasChanged) override;
    void buttonClicked (juce::Button* buttonThatWasClicked) override;

private:
    DexedApvts &apvts_;
    DexedAudioProcessor &proc_;
    AVDexedSettingsStyle::LookAndFeel AVLookAndFeel;
    AudioComponentContainer attachments_;
    std::function<void(ParamDialog *, ParamDialog::TuningAction)> tuning_callback_ = [](ParamDialog *, ParamDialog::TuningAction i) {};
    bool is_standard_tuning_ = true;
    std::function<void(ParamDialog *)> general_callback_ = [](ParamDialog *p) {};

    std::unique_ptr<juce::Slider> pitchRangeUp;
    std::unique_ptr<juce::Slider> pitchRangeDn;
    std::unique_ptr<juce::Slider> pitchStep;

    std::unique_ptr<juce::ComboBox> sysexIn;
    std::unique_ptr<juce::Slider> sysexChl;
    std::unique_ptr<juce::ComboBox> engineType;
    std::unique_ptr<LightedToggleButton> showKeyboard;
    std::unique_ptr<juce::TextButton> sclButton;
    std::unique_ptr<juce::TextButton> kbmButton;
    std::unique_ptr<juce::TextButton> showTunButton;
    std::unique_ptr<juce::TextButton> resetTuningButton;
    std::unique_ptr<juce::Slider> mpePBRange;
    std::unique_ptr<LightedToggleButton> mpeEnabled;
    std::unique_ptr<juce::TextButton> transposeHelp;
    std::unique_ptr<juce::ComboBox> scalingFactor;
    std::unique_ptr<juce::ComboBox> profileSelector;
    std::unique_ptr<juce::TextButton> storeButton;
    // GOZO On/Off pills over the (hidden) toggles they drive.
    std::unique_ptr<AVDexedSettingsStyle::OnOffPills> AVGlissandoPills, AVShowKeyboardPills, AVTransposePills, AVMpePills;
    int AVColumnWidth() const { return (getWidth() - 35) / 2; }
    juce::Component* AVChild(const juce::String& name) const;

    void refreshProfileList();
    void saveProfileToFile(const juce::String& name);
    void loadProfileFromFile(const juce::File& file);
    void AVRefreshLanguage();
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamDialog)
};
