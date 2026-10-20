set(BaseTargetName AVDexed)

set(DEXED_JUCE_FORMATS Standalone)
option(DEXED_COPY_PLUGIN_AFTER_BUILD "Copy built plugin formats into user plugin folders" OFF)

if(NOT DEXED_SKIP_VST3)
  list(APPEND DEXED_JUCE_FORMATS VST3)
endif()

if (APPLE)
  list(APPEND DEXED_JUCE_FORMATS AU)
endif()

message(STATUS "Building Dexed in formats: ${DEXED_JUCE_FORMATS}")

# macOS: the app opens AV Dexed's files from the Finder (a double-click, a drop
# on the Dock icon, "Open with"): sounds (.avdexpreset, and the old .dexedpreset
# name), DX7 banks (.syx, as an alternative to whatever else opens them) and
# folders of them (never offered as an "Open with" choice for folders). They are
# imported like a drop on the window. JUCE merges this into every bundle's
# Info.plist; only the Standalone acts on it. Sounds are claimed through their
# own exported type; banks by extension, because the imported .syx type below
# loses to any installed app that exports one of its own for .syx, and a claim
# on it would then stop matching (the Finder kind string is all it is for).
set(AVDEXED_PLIST_TO_MERGE [=[
<plist version="1.0">
<dict>
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key>
      <string>AV Dexed sound</string>
      <key>CFBundleTypeRole</key>
      <string>Editor</string>
      <key>LSHandlerRank</key>
      <string>Owner</string>
      <key>LSItemContentTypes</key>
      <array>
        <string>org.avvibes.avdexed.sound</string>
      </array>
    </dict>
    <dict>
      <key>CFBundleTypeName</key>
      <string>DX7 SysEx bank</string>
      <key>CFBundleTypeRole</key>
      <string>Viewer</string>
      <key>LSHandlerRank</key>
      <string>Alternate</string>
      <key>CFBundleTypeExtensions</key>
      <array>
        <string>syx</string>
      </array>
    </dict>
    <dict>
      <key>CFBundleTypeName</key>
      <string>Folder of sounds</string>
      <key>CFBundleTypeRole</key>
      <string>Viewer</string>
      <key>LSHandlerRank</key>
      <string>None</string>
      <key>LSItemContentTypes</key>
      <array>
        <string>public.folder</string>
      </array>
    </dict>
  </array>
  <key>UTExportedTypeDeclarations</key>
  <array>
    <dict>
      <key>UTTypeIdentifier</key>
      <string>org.avvibes.avdexed.sound</string>
      <key>UTTypeDescription</key>
      <string>AV Dexed sound</string>
      <key>UTTypeConformsTo</key>
      <array>
        <string>public.data</string>
      </array>
      <key>UTTypeTagSpecification</key>
      <dict>
        <key>public.filename-extension</key>
        <array>
          <string>avdexpreset</string>
          <string>dexedpreset</string>
        </array>
      </dict>
    </dict>
  </array>
  <key>UTImportedTypeDeclarations</key>
  <array>
    <dict>
      <key>UTTypeIdentifier</key>
      <string>org.avvibes.avdexed.sysex-bank</string>
      <key>UTTypeDescription</key>
      <string>DX7 SysEx bank</string>
      <key>UTTypeConformsTo</key>
      <array>
        <string>public.data</string>
      </array>
      <key>UTTypeTagSpecification</key>
      <dict>
        <key>public.filename-extension</key>
        <array>
          <string>syx</string>
        </array>
      </dict>
    </dict>
  </array>
</dict>
</plist>
]=])

juce_add_plugin("${BaseTargetName}"
        VERSION "${PROJECT_VERSION}"
        COMPANY_NAME "AV Vibes"
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        PLUGIN_MANUFACTURER_CODE AVVB
        PLUGIN_CODE AVDx
        FORMATS ${DEXED_JUCE_FORMATS}
        PRODUCT_NAME "AV Dexed"
        DESCRIPTION "AV Dexed free FM instrument"
        BUNDLE_ID "org.avvibes.avdexed"
        ICON_BIG "${PROJECT_SOURCE_DIR}/assets/branding/AVDexed_AppIcon.png"
        ICON_SMALL "${PROJECT_SOURCE_DIR}/assets/branding/AVDexed_AppIcon.png"
        COPY_PLUGIN_AFTER_BUILD ${DEXED_COPY_PLUGIN_AFTER_BUILD}
        PLIST_TO_MERGE "${AVDEXED_PLIST_TO_MERGE}"
)

clap_juce_extensions_plugin(TARGET ${BaseTargetName}
        CLAP_ID "org.avvibes.avdexed"
        CLAP_FEATURES instrument FM)

juce_generate_juce_header(${PROJECT_NAME})

include_directories(../libs/MTS-ESP/Client .)

target_sources(${BaseTargetName} PRIVATE
	msfa/dx7note.cc
	msfa/env.cc
	msfa/exp2.cc
	msfa/fm_core.cc
	msfa/fm_op_kernel.cc
	msfa/freqlut.cc
	msfa/lfo.cc
	msfa/pitchenv.cc
	msfa/sin.cc
	msfa/tuning.cc
	msfa/porta.cpp

	PluginProcessor.cpp
	AVDexedStandaloneApp.cpp

	PluginData.cpp
	PluginFx.cpp
	effects/AVDexedReverb.cpp
	effects/AVDexedChorus.cpp
	effects/AVDexedDelay.cpp
	effects/AVDexedFxChain.cpp
	Tuning.cpp

	core/Cartridge.cpp
	core/AVDexedSysex.cpp
	core/AVDexedPerformance.cpp
	core/AVDexedMicroTuning.cpp
	engine/AVDexedLayer.cpp
	AVDexedLayers.cpp
	core/AVDexedLibrary.cpp
	core/Program.cpp

    engine/EngineMkI.cpp
	engine/EngineOpl.cpp

    ui/PluginEditor.cpp
	ui/AVDexedPlayView.cpp
	ui/AVDexedLayersView.cpp
	ui/AVDexedPromptOverlay.cpp
	ui/AVDexedLanguage.cpp
	ui/AVDexedKeyboard.cpp
	ui/AVDexedResizeGrip.cpp
		ui/window/CartManager.cpp
	ui/DXLookNFeel.cpp
    ui/GlobalEditor.cpp
	ui/OperatorEditor.cpp
		ui/window/ParamDialog.cpp
    ui/window/AVDexedSettingsPanel.cpp
		ui/window/TuningShow.cpp
	ui/component/AlgoDisplay.cpp
	ui/component/DXComponents.cpp
	ui/component/EnvDisplay.cpp
	ui/component/ProgramListBox.cpp
	ui/component/VUMeter.cpp

	midi/SysexComm.cpp
	midi/PluginMidi.cpp

	parameter/DexedApvts.cpp
	parameter/ParametersMapper.cpp
	parameter/State.cpp
    parameter/Preference.cpp

        debugger/ValueTreeDebugger.cpp
        debugger/MemoryLogger.h

    # until MTS-ESP has is own cmake project
    ../libs/MTS-ESP/Client/libMTSClient.cpp
)

if(DEXED_NO_ALSA)
    set(DEXED_ALSA FALSE)
    message(STATUS "Building Dexed without ALSA support")
else()
    set(DEXED_ALSA TRUE)
endif()

find_package(Jack QUIET)

target_compile_definitions(${BaseTargetName} PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_JACK=$<IF:$<BOOL:${Jack_FOUND}>,1,0>
    JUCE_ALSA=$<IF:$<BOOL:${DEXED_ALSA}>,1,0>
    JUCE_ASIO=$<IF:$<PLATFORM_ID:Windows>,1,0> # Windows audio interfaces (JUCE bundles the GPLv3 ASIO SDK)
    JUCE_MODAL_LOOPS_PERMITTED=0 # needed for FileBrowser in CartManager
    JUCE_REPORT_APP_USAGE=0
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
    # JUCE_COREGRAPHICS_RENDER_WITH_MULTIPLE_PAINT_CALLS=1     # REVERT BACK SINCE THERE IS AN ISSUE WITH JUCE 7.0.0
)

target_link_libraries(${BaseTargetName} 
PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_plugin_client
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    surgesynthteam_tuningui
    tuning-library
    DexedResources
PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
)

# The UI text is UTF-8; without this MSVC reads it in the build machine's code page.
target_compile_options(${BaseTargetName} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

if(DEXED_BUILD_TESTS)
    target_compile_definitions(${BaseTargetName} PRIVATE AVDEXED_TESTING=1)
    juce_add_console_app(AVDexedRegressionTests
        PRODUCT_NAME "AV Dexed Regression Tests")
    juce_generate_juce_header(AVDexedRegressionTests)
    target_sources(AVDexedRegressionTests PRIVATE
        ../tests/AVDexedFxChainTests.cpp
        ../tests/AVDexedStateTests.cpp
        ../tests/AVDexedUIEvidence.cpp
        ../tests/AVDexedLibraryTests.cpp
        ../tests/AVDexedDropTests.cpp
        ../tests/AVDexedQALibrary.cpp
        ../tests/AVDexedVoiceTests.cpp
        ../tests/AVDexedGolden.cpp
        ../tests/AVDexedSysexTests.cpp
        ../tests/AVDexedLayerTests.cpp
        ../tests/AVDexedPerformanceTests.cpp)
    target_include_directories(AVDexedRegressionTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../libs
        ${CMAKE_CURRENT_SOURCE_DIR}/../libs/clap-juce-extensions/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../libs/MTS-ESP/Client
        ${CMAKE_CURRENT_SOURCE_DIR}/../libs/tuning-library/include
        ${CMAKE_CURRENT_BINARY_DIR}/AVDexedRegressionTests_artefacts/JuceLibraryCode
        ${CMAKE_CURRENT_BINARY_DIR}/../assets/juce_binarydata_DexedResources/JuceLibraryCode)
    target_compile_definitions(AVDexedRegressionTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        AVDEXED_TESTING=1)
    target_compile_features(AVDexedRegressionTests PRIVATE cxx_std_20)
    target_compile_options(AVDexedRegressionTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(AVDexedRegressionTests PRIVATE
        ${BaseTargetName}
        juce::juce_audio_basics
        juce::juce_dsp
        juce::juce_core
        juce::juce_events
        juce::juce_gui_basics
        juce::juce_recommended_config_flags)
    add_test(NAME AVDexedRegressionTests
        COMMAND $<TARGET_FILE:AVDexedRegressionTests>)
endif()
