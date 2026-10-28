// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <juce_core/system/juce_TargetPlatform.h>
#include "ui/AVDexedLanguage.h"
#include "ui/AVDexedStandaloneAccess.h"

#if JucePlugin_Build_Standalone

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/juce_audio_plugin_client.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace {

juce::File migrateStandaloneSettings(const juce::PropertiesFile::Options& options) {
    const auto destination = options.getDefaultFile();
    if (destination.existsAsFile())
        return destination;

    juce::Array<juce::File> legacyFiles;
    const auto addLegacy = [&legacyFiles, &options](const juce::String& name,
                                                    const juce::String& folder) {
        auto candidate = options;
        candidate.applicationName = name;
        candidate.folderName = folder;
        legacyFiles.addIfNotAlreadyThere(candidate.getDefaultFile());
    };

    // Previous AV Dexed builds used JUCE's default product directory. The
    // upstream standalone build used the product name without a vendor folder.
    addLegacy("AV Dexed", {});
    addLegacy("Dexed", {});

#if JUCE_LINUX || JUCE_BSD
    // Match the path selected by the earlier custom standalone wrapper.
    addLegacy("AV Dexed", "~/.config");
    addLegacy("Dexed", "~/.config");
#endif

    // Also accept the historical app-data layout used by the original product.
    addLegacy("Dexed", "DigitalSuburban/Dexed");

    destination.getParentDirectory().createDirectory();
    for (const auto& legacyFile : legacyFiles) {
        if (legacyFile != destination && legacyFile.existsAsFile()
            && legacyFile.copyFileTo(destination))
            break;
    }

    return destination;
}

class AVDexedStandaloneWindow final : public juce::StandaloneFilterWindow {
public:
    AVDexedStandaloneWindow(const juce::String& title, juce::Colour background,
                            std::unique_ptr<juce::StandalonePluginHolder> holder)
        : juce::StandaloneFilterWindow(title, background, std::move(holder)) {
        // Keep the operating system frame and resizing behavior in the standalone app.
        setResizable(true, false);
        setUsingNativeTitleBar(true);
        if (auto* content = getContentComponent())
            if (content->getWidth() > 0 && content->getHeight() > 0)
                setContentComponentSize(content->getWidth(), content->getHeight());

        // The app has its own audio and preset controls, so hide JUCE's extra Options button.
        for (auto* child : getChildren())
            if (auto* button = dynamic_cast<juce::TextButton*>(child))
                button->setVisible(false);
    }
};

class AVDexedStandaloneApp final : public juce::JUCEApplication, private juce::MenuBarModel {
public:
    AVDexedStandaloneApp() {
        juce::PropertiesFile::Options options;
        options.applicationName = juce::CharPointer_UTF8(JucePlugin_Name);
        options.filenameSuffix = ".settings";
        options.folderName = "AV Vibes/AV Dexed";
        options.osxLibrarySubFolder = "Application Support";
        migrateStandaloneSettings(options);
        appProperties.setStorageParameters(options);
       #if JUCE_WINDOWS
        // A second launch, the foreground app the user just started, lets the
        // app it hands its files to bring its window forward; Windows refuses
        // that to a background app otherwise.
        AllowSetForegroundWindow(ASFW_ANY);
       #endif
    }

    const juce::String getApplicationName() override { return juce::CharPointer_UTF8(JucePlugin_Name); }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    /** Windows: one app, as on macOS. A second launch (a double-click on a
        sound, "Open with") hands its command line to the app already open and
        quits, instead of opening a window of its own that would fight the
        first one for the audio device (an ASIO driver serves one app). Linux
        keeps a window per launch: JUCE cannot pass a command line there. */
    bool moreThanOneInstanceAllowed() override {
       #if JUCE_WINDOWS
        return false;
       #else
        return true;
       #endif
    }
    /** macOS: files the system hands the running app (a double-click in the
        Finder, a drop on the Dock icon, "Open with"). Windows: the command line
        of a second launch; Explorer gives full paths (a relative one, typed in
        a terminal, would be looked for in this app's working folder). On Linux
        files only come on the command line of a launch (initialise). */
    void anotherInstanceStarted(const juce::String& commandLine) override {
       #if JUCE_WINDOWS
        // The launch was the user's: the window they asked for comes forward.
        if (mainWindow != nullptr) {
            if (mainWindow->isMinimised()) mainWindow->setMinimised(false);
            mainWindow->toFront(true);
        }
       #endif
        openFilesFromSystem(commandLine);
    }

    void initialise(const juce::String& commandLine) override {
        mainWindow.reset(createWindow());
        if (mainWindow) {
            AVDexedStandaloneAccess::AVDeviceManager = &mainWindow->pluginHolder->deviceManager;
            AVDexedStandaloneAccess::AVOpenAudioSettings = [] {
                if (AVDexedStandaloneAccess::AVShowProductSettings) AVDexedStandaloneAccess::AVShowProductSettings();
            };
        }
#if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu(this);
#elif ! JUCE_WINDOWS
        // Windows: no unthemed JUCE bar in the window; Settings stay on the gear.
        if (mainWindow) mainWindow->setMenuBar(this);
#endif
        if (mainWindow != nullptr) {
#if JUCE_STANDALONE_FILTER_WINDOW_USE_KIOSK_MODE
            juce::Desktop::getInstance().setKioskModeComponent(mainWindow.get(), false);
#endif
            mainWindow->setVisible(true);
        } else {
            pluginHolder = createPluginHolder();
        }
        // Files on the command line (a Windows or Linux launch with files); on
        // macOS they arrive just after this, through anotherInstanceStarted.
        openFilesFromSystem(commandLine);
    }

    void shutdown() override {
#if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu(nullptr);
#elif ! JUCE_WINDOWS
        if (mainWindow) mainWindow->setMenuBar(nullptr);
#endif
        AVDexedStandaloneAccess::AVDeviceManager = nullptr;
        AVDexedStandaloneAccess::AVOpenAudioSettings = {};
        pluginHolder = nullptr;
        mainWindow = nullptr;
        appProperties.saveIfNeeded();
    }

    void systemRequestedQuit() override {
        if (pluginHolder != nullptr)
            pluginHolder->savePluginState();
        if (mainWindow != nullptr)
            mainWindow->pluginHolder->savePluginState();

        if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents()) {
            juce::Timer::callAfterDelay(100, [] {
                if (auto* app = juce::JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
        } else {
            quit();
        }
    }

private:
    /** Hands the paths in `commandLine` to the editor, which imports them like a
        drop on the window. The editor is created with the window, so it is
        there when files arrive; should it not be yet, this looks again a few
        times before giving up. */
    void openFilesFromSystem(const juce::String& commandLine, int attempt = 0) {
        const auto paths = AVDexedStandaloneAccess::AVSystemOpenPaths(commandLine);
        if (paths.isEmpty()) return;
        if (AVDexedStandaloneAccess::AVOpenFiles) {
            AVDexedStandaloneAccess::AVOpenFiles(paths);
            return;
        }
        if (attempt < 20)
            juce::Timer::callAfterDelay(250, [commandLine, attempt] {
                if (auto* app = dynamic_cast<AVDexedStandaloneApp*>(juce::JUCEApplication::getInstance()))
                    app->openFilesFromSystem(commandLine, attempt + 1);
            });
    }

    juce::StringArray getMenuBarNames() override { return {"Audio / MIDI"}; }
    juce::PopupMenu getMenuForIndex(int, const juce::String&) override {
        juce::PopupMenu menu;
        menu.addItem(1, AVDexedLanguageService::getInstance().text("audioMidiSettings"));
        return menu;
    }
    void menuItemSelected(int item, int) override {
        if (item == 1 && AVDexedStandaloneAccess::AVShowProductSettings)
            AVDexedStandaloneAccess::AVShowProductSettings();
    }
    AVDexedStandaloneWindow* createWindow() {
        if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
            return nullptr;
        return new AVDexedStandaloneWindow(
            getApplicationName(),
            juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
            createPluginHolder());
    }

    std::unique_ptr<juce::StandalonePluginHolder> createPluginHolder() {
        constexpr auto autoOpenMidiDevices =
#if (JUCE_ANDROID || JUCE_IOS) && ! JUCE_DONT_AUTO_OPEN_MIDI_DEVICES_ON_MOBILE
            true;
#else
            false;
#endif

#ifdef JucePlugin_PreferredChannelConfigurations
        constexpr juce::StandalonePluginHolder::PluginInOuts channels[] { JucePlugin_PreferredChannelConfigurations };
        const juce::Array<juce::StandalonePluginHolder::PluginInOuts> channelConfig(
            channels, juce::numElementsInArray(channels));
#else
        const juce::Array<juce::StandalonePluginHolder::PluginInOuts> channelConfig;
#endif

        return std::make_unique<juce::StandalonePluginHolder>(appProperties.getUserSettings(), false,
                                                               juce::String{}, nullptr, channelConfig,
                                                               autoOpenMidiDevices);
    }

    juce::ApplicationProperties appProperties;
    std::unique_ptr<AVDexedStandaloneWindow> mainWindow;
    std::unique_ptr<juce::StandalonePluginHolder> pluginHolder;
};

} // namespace

START_JUCE_APPLICATION(AVDexedStandaloneApp)

#endif
