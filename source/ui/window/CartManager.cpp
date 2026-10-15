/**
 *
 * Copyright (c) 2015-2024 Pascal Gauthier.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 */

#include "PluginProcessor.h"
#include "../PluginEditor.h"
#include "../DXLookNFeel.h"
#include "CartManager.h"
#include "../AVDexedLanguage.h"
#include "../AVDexedTheme.h"
#include "PluginData.h"
#include "../../core/AVDexedLibrary.h"

#include <fstream>

class SyxFileFilter : public FileFilter {
public:
    SyxFileFilter() : FileFilter(".syx") {}
    // The library's hidden folders (.migration-backup, .profiles, .deleted) are
    // not hidden files on Windows: filter them by name as the library does.
    bool isFileSuitable(const File &file) const override {
        return file.getFileExtension().toLowerCase() == ".syx" && file.getSize() >= 4096
            && ! AVDexedLibrary::isIgnoredName(file.getFileName());
    }
    bool isDirectorySuitable(const File &file) const override {
        return ! AVDexedLibrary::isIgnoredName(file.getFileName());
    };
};

/** The bank tree takes no file drops itself (a TreeView accepts every drag):
    they reach the editor, which shows its drop wash and imports them into the
    library the same way as everywhere else (folder prompt, never overwriting). */
class FileTreeDrop : public FileTreeComponent {
public :
    FileTreeDrop(DirectoryContentsList &listToShow) : FileTreeComponent(listToShow) {}

    bool isInterestedInFileDrag (const StringArray&) override { return false; }
};

/**
 * This is a hack to override global dexed focus traversal when the cart manager is shown. We could have used a 
 * modal window but this would have blocked the keys and the user would not have been able to play the current
 * selected preset. Anything that is keyboard related should be ordered in the focusOrder vector.
 */
class CartBrowserFocusTraverser : public KeyboardFocusTraverser {
    std::vector<Component*> &orders;
    Component *root;
public:
    CartBrowserFocusTraverser(Component *root, std::vector<Component*> &orders) : orders(orders), root(root) {}

    Component* getDefaultComponent(Component* parentComponent) override {
        return orders[0];
    }

    Component* getNextComponent(Component* current) override {
        bool srcFound = false;
        int i;

        for (i=0;i<orders.size();i++) {
            if ( orders[i] == current ) {
                srcFound = true;
                continue;
            }

            if ( srcFound ) {
                ProgramLabel *label = dynamic_cast<ProgramLabel*>(orders[i]);
                if ( label != nullptr && !label->isActive() )
                    continue;
                break;
            }
        }

        if ( i == orders.size() )
            return orders.front();
        return orders[i];
    }

    Component* getPreviousComponent(Component* current) override {
        bool srcFound = false;
        int i=0;

        for(i=orders.size()-1;i>=0;i--) {
            if ( orders[i] == current ) {
                srcFound = true;
                continue;
            }
            if ( srcFound ) {
                ProgramLabel *label = dynamic_cast<ProgramLabel*>(orders[i]);
                if ( label != nullptr && !label->isActive() )
                    continue;
                break;
            }
        }
        if ( i == -1 )
            return orders.back();
        return orders[i];
    }

    std::vector<Component*> getAllComponents(Component* parentComponent) override {
        return orders;
    }
};

CartManager::CartManager(DexedAudioProcessorEditor *editor) : Component("CartManager") {
    mainWindow = editor;
    // The data folder is the library: its folders are the tree's folders.
    cartDir = AVDexedLibrary::rootFor(DexedAudioProcessor::dexedAppDir);

    activeCart.reset(new ProgramListBox("Active Programs Selector", 8));
    addAndMakeVisible(activeCart.get());
    activeCart->addListener(this);

    browserCart.reset(new ProgramListBox("Browser Programs Selector", 2));
    addAndMakeVisible(browserCart.get());
    browserCart->addListener(this);

    // -------------------------
    syxFileFilter.reset(new SyxFileFilter());
    timeSliceThread.reset(new TimeSliceThread("Cartridge Directory Scanner"));
    timeSliceThread->startThread();

    cartBrowserList.reset(new DirectoryContentsList(syxFileFilter.get(), *timeSliceThread.get()));
    cartBrowserList->setDirectory(cartDir, true, true);
    cartBrowser.reset(new FileTreeDrop(*cartBrowserList));
    cartBrowser->addKeyListener(this);
    addAndMakeVisible(cartBrowser.get());

    cartBrowser->setTitle("Cartridge file browser");
    cartBrowser->setDragAndDropDescription("Sysex Browser");
    cartBrowser->addListener(this);

    closeButton.reset(new TextButton("CLOSE"));
    addAndMakeVisible(closeButton.get());
    closeButton->addListener(this);

    loadButton.reset(new TextButton("LOAD"));
    addAndMakeVisible(loadButton.get());
    loadButton->addListener(this);

    saveButton.reset(new TextButton("SAVE"));
    addAndMakeVisible(saveButton.get());
    saveButton->addListener(this);

    fileMgrButton.reset(new TextButton("SHOW DIR"));
    addAndMakeVisible(fileMgrButton.get());
    fileMgrButton->addListener(this);

    activeCartName.reset(new CartridgeFileDisplay());
    addAndMakeVisible(activeCartName.get());
    AVDexedLanguageService::getInstance().addChangeListener(this);
    AVRefreshLanguage();

    focusOrder.push_back(cartBrowser.get());
    //focusOrder.push_back(browserCart.get());
    for(int i=0;i<32;i++) {
        focusOrder.push_back(browserCart->getProgramComponent(i));
    }
    //focusOrder.push_back(activeCart.get());
    for(int i=0;i<32;i++) {
        focusOrder.push_back(activeCart->getProgramComponent(i));
    }
    focusOrder.push_back(closeButton.get());
    focusOrder.push_back(loadButton.get());
    focusOrder.push_back(saveButton.get());
    focusOrder.push_back(fileMgrButton.get());

/*
 *
 * I've removed this since it only works on the DX7 II. TBC.
 *
    addAndMakeVisible(getDXPgmButton = new TextButton("GET DEXED VOICE"));
    getDXPgmButton->setBounds(656, 545, 100, 30);
    getDXPgmButton->addListener(this);

    addAndMakeVisible(getDXCartButton = new TextButton("GET DEXED BANK"));
    getDXCartButton->setBounds(755, 545, 100, 30);
    getDXCartButton->addListener(this);
 */
}

CartManager::~CartManager() {
    AVDexedLanguageService::getInstance().removeChangeListener(this);
    timeSliceThread->stopThread(500);
    cartBrowser.reset(NULL);
    cartBrowserList.reset(NULL);
}

std::unique_ptr<ComponentTraverser> CartManager::createKeyboardFocusTraverser() {
    return std::make_unique<CartBrowserFocusTraverser>(this, focusOrder);
}

void CartManager::resized() {
    const int inset = 16;
    const int top = 44;
    const int bottom = getHeight() - 56;
    const int activeHeight = jlimit(128, 156, getHeight() / 4);
    const int activeY = bottom - activeHeight;
    const int previewWidth = jmax(280, getWidth() * 36 / 100);
    const int upperHeight = jmax(120, activeY - top - 32);
    cartBrowser->setBounds(inset, top, getWidth() - previewWidth - inset * 3, upperHeight);
    browserCart->setBounds(getWidth() - inset - previewWidth, top, previewWidth, upperHeight);
    activeCart->setBounds(inset, activeY, getWidth() - inset * 2, activeHeight);
    const int buttonY = getHeight() - 42;
    closeButton->setBounds(inset, buttonY, 92, 30);
    loadButton->setBounds(inset + 100, buttonY, 104, 30);
    saveButton->setBounds(inset + 212, buttonY, 104, 30);
    fileMgrButton->setBounds(inset + 324, buttonY, 136, 30);
    activeCartName->setBounds(inset + 474, buttonY, jmax(80, getWidth() - inset * 2 - 474), 30);
}

void CartManager::paint(Graphics& g) {
    AVDexedTheme::panel(g, getLocalBounds().toFloat(), 10.0f);
    auto& language = AVDexedLanguageService::getInstance();
    const auto heading = AVDexedTheme::font(15.0f, "SemiBold");
    AVDexedTheme::drawGreyText(g, language.text("bankFiles"), heading,
                               Rectangle<int>(16, 10, cartBrowser->getWidth(), 28).toFloat(), Justification::centredLeft);
    AVDexedTheme::drawGreyText(g, language.text("bankPreview"), heading,
                               Rectangle<int>(browserCart->getX(), 10, browserCart->getWidth(), 28).toFloat(), Justification::centredLeft);
    AVDexedTheme::drawGreyText(g, language.text("currentBank"), heading,
                               Rectangle<int>(16, activeCart->getY() - 30, getWidth() - 32, 28).toFloat(), Justification::centredLeft);
    // Neutral wells behind the file tree and the preview, instead of black holes.
    AVDexedTheme::fillBox(g, cartBrowser->getBounds().toFloat().expanded(2.0f), 6.0f, false, 0.6f);
    AVDexedTheme::fillBox(g, browserCart->getBounds().toFloat().expanded(2.0f), 6.0f, false, 0.6f);
}

void CartManager::changeListenerCallback(ChangeBroadcaster*) { AVRefreshLanguage(); }

void CartManager::AVRefreshLanguage() {
    auto& language = AVDexedLanguageService::getInstance();
    closeButton->setButtonText(language.text("close"));
    loadButton->setButtonText(language.text("load"));
    saveButton->setButtonText(language.text("save"));
    fileMgrButton->setButtonText(language.text("showDirectory"));
    cartBrowser->setTitle(language.text("bankBrowser"));
    activeCart->setTitle(language.text("activePrograms"));
    browserCart->setTitle(language.text("browserPrograms"));
    for (auto* button : { closeButton.get(), loadButton.get(), saveButton.get(), fileMgrButton.get() }) {
        button->setTitle(button->getButtonText());
        button->setWantsKeyboardFocus(true);
    }
    repaint();
}

void CartManager::updateCartFilename() {
    activeCartName->setCartrdigeFile(mainWindow->processor->activeFileCartridge);
}

void CartManager::programSelected(ProgramListBox* source, int pos) {
    if (pos < 0 || pos >= 32) return;
    const bool currentBank = source == activeCart.get();
    auto selectedCart = source->getCurrentCart();
    // The preview's own file: the tree's selection may have moved to a folder.
    const auto sourceFile = AVPreviewFile;
    const auto safe = Component::SafePointer<CartManager>(this);
    mainWindow->requestPresetReplacement([safe, selectedCart, sourceFile, pos, currentBank]() mutable {
        if (safe == nullptr) return;
        auto* editor = safe->mainWindow;
        if (!currentBank) {
            // Before setCurrentProgram: it applies the sound settings of this file.
            // The preview was read when its file was selected; a "Guardar" into
            // that bank since then (even the one the save-changes prompt just
            // made) is on disk, and installing the older copy would undo it here.
            editor->processor->setActiveBank(AVDexedLibrary::currentBankOf(sourceFile, selectedCart), sourceFile);
            safe->activeCart->setCartridge(editor->processor->currentCart);
            if (sourceFile == safe->AVPreviewFile)
                safe->browserCart->setCartridge(editor->processor->currentCart);
        }
        safe->browserCart->setActive(-1);
        editor->processor->setCurrentProgram(pos);
        editor->processor->updateHostDisplay();
        editor->updateUI();
        safe->updateCartFilename();
        safe->repaint();
    });
}

void CartManager::buttonClicked(juce::Button *buttonThatWasClicked) {
    if ( buttonThatWasClicked == closeButton.get() ) {
        hideCartridgeManager();
        return;
    }

    if ( buttonThatWasClicked == loadButton.get() ) {
        // Banks usually arrive in Downloads. One from outside the library is
        // imported (copied into a library folder) instead of played from there.
        auto start = File::getSpecialLocation(File::userHomeDirectory).getChildFile("Downloads");
        if (! start.isDirectory()) start = File::getSpecialLocation(File::userDocumentsDirectory);
        fileChooser = std::make_unique<FileChooser>(AVDexedLanguageService::getInstance().text("importBankTitle"), start, "*.syx;*.SYX", true);

        const auto safe = Component::SafePointer<CartManager>(this);
        fileChooser->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                 [safe](const FileChooser& fc) {
                                     if ( safe == nullptr || fc.getResults().isEmpty() )
                                         return;
                                     // The "Based on" label follows when the bank is
                                     // committed: an import or a save-changes prompt
                                     // may come first.
                                     const auto file = fc.getResult();
                                     if (file.isAChildOf(safe->cartDir))
                                         safe->mainWindow->loadCart(file);
                                     else
                                         safe->mainWindow->importBanks({ file });
                                 });
        return;
    }

    if ( buttonThatWasClicked == saveButton.get() ) {
        mainWindow->saveCart();
    }

    if ( buttonThatWasClicked == fileMgrButton.get() ) {
        cartDir.revealToUser();
        return;
    }

    // THIS IS NOT USED
    if ( buttonThatWasClicked == getDXPgmButton.get() ) {
        if ( mainWindow->processor->sysexComm.isInputActive() && mainWindow->processor->sysexComm.isOutputActive() ) {
            unsigned char msg[] = { 0xF0, 0x43, 0x20, 0x09, 0xF7 };
            mainWindow->processor->sysexComm.send(MidiMessage(msg, 5));
        } else {
            showSysexConfigMsg();
        }
        return;
    }

    if ( buttonThatWasClicked == getDXCartButton.get() ) {
        if ( mainWindow->processor->sysexComm.isInputActive() && mainWindow->processor->sysexComm.isOutputActive() ) {
            unsigned char msg[] = { 0xF0, 0x43, 0x20, 0x00, 0xF7 };
            mainWindow->processor->sysexComm.send(MidiMessage(msg, 5));
        } else {
            showSysexConfigMsg();
        }
        return;
    }
}

void CartManager::fileDoubleClicked(const File& file) {
    if ( file.isDirectory() )
        return;
    mainWindow->loadCart(file);
    activeCart->setCartridge(mainWindow->processor->currentCart);
    updateCartFilename();
}

void CartManager::fileClicked(const File& file, const MouseEvent& e) {
    if ( e.mods.isPopupMenu()) {
        PopupMenu menu;

        menu.addItem(1000, AVDexedLanguageService::getInstance().text("openLocation"));
        if ( ! file.isDirectory() ) {
            menu.addItem(1010, AVDexedLanguageService::getInstance().text("sendBank"));
        }
        menu.addSeparator();
        menu.addItem(1020, AVDexedLanguageService::getInstance().text("refresh"));

        const auto safe = Component::SafePointer<CartManager>(this);
        menu.setLookAndFeel(&getLookAndFeel());
        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(this), [safe, file](int result) {
            if (safe == nullptr) return;
            switch(result) {
            case 1000:
                file.revealToUser();
                break;
            case 1010 :
                safe->mainWindow->processor->sendSysexCartridge(file);
                break;
            case 1020:
                safe->cartBrowserList->refresh();
                break;
            }
        });
        return;
    }
}

void CartManager::setActiveProgram(int idx, String activeName) {
    if (idx >= 0 && idx < activeCart->programNames.size() && activeCart->programNames[idx] == activeName) {
        activeCart->setActive(idx);
        browserCart->setActive(-1);
    }
    activeCart->repaint();
}

void CartManager::resetActiveSysex() {
    activeCart->setCartridge(mainWindow->processor->currentCart);
}

void CartManager::selectionChanged() {
    File file = cartBrowser->getSelectedFile();

    if ( ! file.exists() )
        return;

    if ( file.isDirectory() )
        return;

    if (! AVLoadPreview(file)) {
        auto& language = AVDexedLanguageService::getInstance();
        mainWindow->showAVNotice(language.text("error"), language.format("bankOpenError", {{"path", file.getFullPathName()}}));
        return;
    }
    browserCart->setActive(-1);
}

bool CartManager::AVLoadPreview(const File& file) {
    Cartridge browserSysex;
    const int rc = browserSysex.load(file);
    if ( rc < 0 )
        return false;
    // A damaged or non-bank file is shown but never written to.
    browserCart->readOnly = rc != 0;
    AVPreviewFile = file;
    browserCart->setCartridge(browserSysex);
    return true;
}

void CartManager::reloadPreview() {
    if (AVPreviewFile.existsAsFile())
        AVLoadPreview(AVPreviewFile);
}

void CartManager::programRightClicked(ProgramListBox *source, int pos) {
    PopupMenu menu;

    menu.addItem(1000, AVDexedLanguageService::getInstance().format("sendVoice", {{ "name", source->programNames[pos] }}));

    if ( source == activeCart.get() )
        menu.addItem(1010, AVDexedLanguageService::getInstance().text("sendCurrentBank"));

    const auto safe = Component::SafePointer<CartManager>(this);
    const auto safeSource = Component::SafePointer<ProgramListBox>(source);
    menu.setLookAndFeel(&getLookAndFeel());
    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(source), [safe, safeSource, pos](int result) {
        if (safe == nullptr || safeSource == nullptr) return;
        auto* source = safeSource.getComponent();
        auto* mainWindow = safe->mainWindow;
        switch(result)  {
            case 1000: {
                Program program;

                if ( source == safe->activeCart.get() ) {
                    program = mainWindow->processor->currentCart.unpackProgram(pos);
                } else {
                    program = source->getCurrentCart().unpackProgram(pos);
                }

                if ( mainWindow->processor->sysexComm.isOutputActive() ) {
                    uint8_t msg[163];
                    exportSysexPgm(msg, program.getRawData());
                    msg[2] |= mainWindow->processor->sysexComm.getChl();
                    mainWindow->processor->sysexComm.send(MidiMessage(msg, 163));
                }
                break;
            }
            case 1010:
                mainWindow->processor->sendCurrentSysexCartridge();
                break;
        }
    });
}

void CartManager::programDragged(ProgramListBox* destListBox, int dest, char* packedPgm) {
    if (packedPgm == nullptr || dest < 0 || dest >= 32) return;
    auto* processor = mainWindow->processor;
    if (destListBox == activeCart.get()) {
        const auto previous = processor->currentCart;
        auto updated = previous;
        updated.setPackedProgram(dest, packedPgm);
        mainWindow->playView.carryAVBankMetadata(previous, updated);
        // Through setActiveBank: it swaps the bank under the audio lock (a MIDI
        // program change reads it). Same file: only the memory copy changes, so
        // "Guardar" later sees the difference and offers "Guardar como".
        const auto bankFile = processor->activeFileCartridge;
        processor->setActiveBank(updated, bankFile);
        processor->updateHostDisplay();
        mainWindow->updateUI();
    } else {
        // The file the preview shows (the tree's selection may be a folder now).
        const auto file = AVPreviewFile;
        if (!file.existsAsFile()) return;
        auto& language = AVDexedLanguageService::getInstance();
        // Factory and expansion packs are read-only.
        if (AVDexedLibrary::isReadOnly(file, cartDir)) {
            mainWindow->showAVNotice(language.text("saveError"), language.text("packFolderReadOnly"));
            return;
        }
        // Only a file that is one bank is changed in place: an archive of
        // several banks (64 KB or more) is left exactly as it is.
        Cartridge cart;
        if (! AVDexedLibrary::readBank(file, cart)) {
            mainWindow->showAVNotice(language.text("saveError"), language.format("bankNotRewritable", {{"bank", file.getFileName()}}));
            return;
        }
        const auto previous = cart;
        cart.setPackedProgram(dest, packedPgm);
        // Only the bank message is replaced: the sound settings embedded in the
        // file stay (the slot that changed simply has none for its new voice).
        if (! DexedAudioProcessorEditor::AVWriteBankFile(cart, file)) {
            mainWindow->showAVNotice(language.text("saveError"), language.format("writeError", {{"path", file.getFullPathName()}}));
            return;
        }
        mainWindow->playView.carryAVBankMetadata(previous, cart);
        browserCart->setCartridge(cart);
        // The bank that is playing: its copy in memory follows the file (the
        // sound being played and its settings stay as they are), unless that
        // copy holds changes of its own, which must not be thrown away.
        if (file == processor->activeFileCartridge
            && AVDexedLibrary::bankFingerprint(processor->currentCart) == AVDexedLibrary::bankFingerprint(previous)) {
            processor->setActiveBank(cart, file);
            processor->updateHostDisplay();
            mainWindow->updateUI();
        }
        mainWindow->playView.refreshLibrary();
    }
}

void CartManager::initialFocus() {
    cartBrowser->grabKeyboardFocus();
}

void CartManager::hideCartridgeManager() {
        // The editor's own rate (meters, footer, resize countdown count 20 ms ticks).
        mainWindow->startTimer(20);
        getParentComponent()->setVisible(false);
}

bool CartManager::keyPressed(const KeyPress& key, Component* originatingComponent) {
    if ( key.getKeyCode() == KeyPress::returnKey ) {
        File file = cartBrowser->getSelectedFile();
        if ( file.isDirectory() )
            return true;
        mainWindow->loadCart(file);
        activeCart->setCartridge(mainWindow->processor->currentCart);
        return true;
    }
    return false;
}

void CartManager::showSysexConfigMsg() {
    auto& language = AVDexedLanguageService::getInstance();
    mainWindow->showAVNotice(language.text("warning"), language.text("sysexConfigurationWarning"));
}

void CartManager::refreshFiles() {
    if (cartBrowserList != nullptr) cartBrowserList->refresh();
}

// unused stuff from FileBrowserListener
void CartManager::browserRootChanged (const File& newRoot) {}
