# Linker options for Linux
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_options(-fPIC)
endif()

juce_add_binary_data(DexedResources SOURCES
    fonts/Montserrat-ExtraLight.ttf
    fonts/Montserrat-Light.ttf
    fonts/Montserrat-Regular.ttf
    fonts/Montserrat-Medium.ttf
    fonts/Montserrat-SemiBold.ttf
    fonts/Montserrat-Bold.ttf
    builtin_pgm.zip
    lang/en.txt
    lang/es.txt
    branding/AVDexed_Logo.svg
    branding/AVDexed_InfoIcon.svg
    branding/AVDexed_PanicIcon.svg
    branding/AVDexed_FavoriteIcon.svg
    branding/AVDexed_FolderIcon.svg
    branding/AVDexed_ResizeIcon.svg
    branding/AVDexed_PreviousPreset.svg
    branding/AVDexed_NextPreset.svg
    branding/AVDexed_SaveIcon.svg
    branding/AVDexed_SettingsIcon.svg
    branding/AVDexed_CloseIcon.svg
    branding/AVDexed_PlusIcon.svg
)
