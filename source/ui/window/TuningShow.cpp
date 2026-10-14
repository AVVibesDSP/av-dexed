/*
  ==============================================================================

  This is an automatically generated GUI class created by the Projucer!

  Be careful when adding custom code to these files, as only the code within
  the "//[xyz]" and "//[/xyz]" sections will be retained when the file is loaded
  and re-saved.

  Created with Projucer version: 5.4.7

  ------------------------------------------------------------------------------

  The Projucer is part of the JUCE library.
  Copyright (c) 2017 - ROLI Ltd.

  ==============================================================================
*/

//[Headers] You can add your own extra header files here...
//[/Headers]

#include "TuningShow.h"
#include "../AVDexedTheme.h"


namespace {
class AVTuningTableModel final : public surgesynthteam::TuningTableListBoxModel {
public:
    AVTuningTableModel(TableListBox& table, const Tunings::Tuning& tuning) : AVTable(&table), AVTuning(tuning) {}
    void paintRowBackground(Graphics& g, int row, int, int, bool selected) override {
        g.fillAll(selected ? AVDexedTheme::oceanBottom : row % 2 == 0 ? AVDexedTheme::controlBottom : AVDexedTheme::panelTop);
    }
    void paintCell(Graphics& g, int row, int column, int width, int height, bool) override {
        static const StringArray names { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        String text;
        switch (column) {
            case surgesynthteam::COLUMNID_NOTE: text = String(row); break;
            case surgesynthteam::COLUMNID_NAME: text = names[row % 12] + String(row / 12 - 1); break;
            case surgesynthteam::COLUMNID_FREQ: text = String(AVTuning.frequencyForMidiNote(row), 4); break;
            case surgesynthteam::COLUMNID_LOG2F: text = String(AVTuning.logScaledFrequencyForMidiNote(row), 6); break;
            default: break;
        }
        g.setColour(AVDexedTheme::textPrimary);
        g.setFont(AVDexedTheme::font(11.0f));
        g.drawText(text, 10, 0, width - 20, height, Justification::centredLeft);
    }
    void cellClicked(int, int, const MouseEvent& event) override {
        if (!event.mods.isPopupMenu()) return;
        PopupMenu menu;
        menu.addItem(1, AVDexedLanguageService::getInstance().text("exportCsv"));
        const auto table = AVTable;
        menu.setLookAndFeel(&table->getLookAndFeel());
        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(table.getComponent()), [this, table](int result) {
            if (table != nullptr && result == 1) exportToCSV();
        });
    }
    void exportToCSV() override {
        AVChooser = std::make_unique<FileChooser>(AVDexedLanguageService::getInstance().text("exportCsvTitle"), File(), "*.csv");
        const auto tuning = AVTuning;
        const auto table = AVTable;
        AVChooser->launchAsync(FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
            [tuning, table](const FileChooser& chooser) {
                if (table == nullptr || chooser.getResults().isEmpty()) return;
                const auto destination = chooser.getResult().withFileExtension(".csv");
                String csv("MIDI note,Frequency (Hz),log2(f/8.17)\n");
                for (int note = 0; note < 128; ++note)
                    csv += String(note) + "," + String(tuning.frequencyForMidiNote(note), 9) + "," + String(tuning.logScaledFrequencyForMidiNote(note), 9) + "\n";
                TemporaryFile temporary(destination);
                if (!temporary.getFile().replaceWithText(csv) || !temporary.overwriteTargetFileWithTemporary()) {
                    auto& language = AVDexedLanguageService::getInstance();
                    AVShowMessage(AlertWindow::WarningIcon, language.text("saveError"),
                                  language.format("writeError", {{"path", destination.getFullPathName()}}), table.getComponent());
                }
            });
    }
private:
    Component::SafePointer<TableListBox> AVTable;
    Tunings::Tuning AVTuning;
    std::unique_ptr<FileChooser> AVChooser;
};
}


//==============================================================================
TuningShow::TuningShow () {
    //[Constructor_pre] You can add your own custom stuff here..
    //[/Constructor_pre]

    tabbedComponent.reset (new TabbedComponent (TabbedButtonBar::TabsAtTop));
    addAndMakeVisible (tabbedComponent.get());
    setLookAndFeel(&AVLookAndFeel);
    tabbedComponent->setTabBarDepth(36);
    tabbedComponent->addTab (AVDexedLanguageService::getInstance().text("tuning"), AVDexedTheme::panelTop, new TableListBox(), true, 0);
    tabbedComponent->addTab ("SCL", AVDexedTheme::panelTop, new TextEditor(), true, 1);
    tabbedComponent->addTab ("KBM", AVDexedTheme::panelTop, new TextEditor(), true, 2);
    tabbedComponent->setCurrentTabIndex (0);

    tabbedComponent->setBounds (0, 0, 500, 400);


    //[UserPreSize]
    //[/UserPreSize]

    setSize(620, 440);


    //[Constructor] You can add your own custom stuff here..
    sclt = dynamic_cast<TextEditor*>( tabbedComponent->getTabContentComponent(1) );
    kbmt = dynamic_cast<TextEditor*>( tabbedComponent->getTabContentComponent(2) );
    sclt->setReadOnly(true);
    kbmt->setReadOnly(true);

    sclt->setMultiLine(true);
    sclt->setScrollbarsShown(true);
    kbmt->setMultiLine(true);
    kbmt->setScrollbarsShown(true);

    table = dynamic_cast<TableListBox*>(tabbedComponent->getTabContentComponent(0));
    table->setRowHeight(28);
    sclt->setFont(AVDexedTheme::font(11.5f, "Regular"));
    kbmt->setFont(AVDexedTheme::font(11.5f, "Regular"));
    AVDexedLanguageService::getInstance().addChangeListener(this);
    AVRefreshLanguage();
    resized();

    //[/Constructor]
}

TuningShow::~TuningShow() {
    //[Destructor_pre]. You can add your own custom destruction code here..
    //[/Destructor_pre]

    AVDexedLanguageService::getInstance().removeChangeListener(this);
    table->setModel(nullptr);
    mod.reset();
    setLookAndFeel(nullptr);
    tabbedComponent = nullptr;


    //[Destructor]. You can add your own custom destruction code here..
    //[/Destructor]
}

//==============================================================================
void TuningShow::paint (Graphics& g) {
    //[UserPrePaint] Add your own custom painting code here..
    //[/UserPrePaint]

    g.fillAll(AVDexedTheme::canvas);

    //[UserPaint] Add your own custom painting code here..
    //[/UserPaint]
}

void TuningShow::resized() {
    if (tabbedComponent != nullptr) tabbedComponent->setBounds(getLocalBounds().reduced(8));
    //[UserPreResize] Add your own custom resize code here..
    //[/UserPreResize]

    //[UserResized] Add your own custom resize handling here..
    //[/UserResized]
}



//[MiscUserCode] You can add your own definitions of your custom methods or any other code here...
void TuningShow::setTuning( const Tunings::Tuning &t ) {
    tuning = t;
    if( sclt )
        sclt->setText( tuning.scale.rawText, false );
    if( kbmt )
        kbmt->setText( tuning.keyboardMapping.rawText, false );

    mod = std::make_unique<AVTuningTableModel>(*table, tuning);
    mod->setTableListBox( table );
    AVRefreshLanguage();
    mod->tuningUpdated( tuning );

    table->setModel( mod.get() );

    table->getViewport()->setScrollBarsShown(true,false);
    table->getViewport()->setViewPositionProportionately( 0.0,  60.0 / 127.0 );
}
//[/MiscUserCode]


void TuningShow::changeListenerCallback(ChangeBroadcaster*) { AVRefreshLanguage(); }

void TuningShow::AVRefreshLanguage() {
    auto& language = AVDexedLanguageService::getInstance();
    tabbedComponent->setTabName(0, language.text("tuning"));
    sclt->setTitle(language.text("loadScale"));
    kbmt->setTitle(language.text("loadKeyboardMap"));
    table->setTitle(language.text("showTuning"));
    auto& header = table->getHeader();
    header.removeAllColumns();
    header.addColumn(language.text("tuningNote"), surgesynthteam::COLUMNID_NOTE, 92, 70, -1, TableHeaderComponent::notSortable);
    header.addColumn(language.text("tuningNoteName"), surgesynthteam::COLUMNID_NAME, 126, 90, -1, TableHeaderComponent::notSortable);
    header.addColumn(language.text("tuningFrequency"), surgesynthteam::COLUMNID_FREQ, 180, 150, -1, TableHeaderComponent::notSortable);
    header.addColumn("log2(f/8.17)", surgesynthteam::COLUMNID_LOG2F, 164, 140, -1, TableHeaderComponent::notSortable);
    header.setPopupMenuActive(false);
    repaint();
}
