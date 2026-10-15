#ifndef Publisher
#define Publisher "AV Vibes"
#endif

#ifndef Version
#define Version "0.0.0"
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
#ifdef LicenseEs
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"; LicenseFile: "{#LicenseEs}"
#else
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
#endif

[Setup]
; Fixed, so that a new version replaces the old one. Not shared with the original Dexed.
AppId={{3366F2A8-7C7B-4D0B-8302-51393B5A116E}
OutputBaseFilename={#NameCondensed}-{#Version}-win
AppName={#Name}
AppPublisher={#Publisher}
AppPublisherURL=https://github.com/AVVibesDSP/av-dexed
AppSupportURL=https://github.com/AVVibesDSP/av-dexed/issues
AppVerName={#Name} {#Version}
AppVersion={#Version}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplicationsFilter=*.exe,*.vst3,*.clap
DefaultDirName={autopf}\{#Name}\
DefaultGroupName={#Name}
DisableDirPage=no
DisableProgramGroupPage=yes
; English or Spanish, following the Windows language.
ShowLanguageDialog=auto
#ifdef License
LicenseFile={#License}
#endif
#ifdef Icon
SetupIconFile={#Icon}
#endif
SetupLogging=yes
SolidCompression=yes
Compression=zip
UninstallDisplayIcon={uninstallexe}
UninstallFilesDir={autoappdata}\{#Name}\uninstall
WizardStyle=modern
WizardSizePercent=100

; Enable non-elevated install. Disabled until we resolve issues with DAWs.
; PrivilegesRequired=lowest
; PrivilegesRequiredOverridesAllowed=dialog

; Only the program and the plug-ins are installed. The sounds and settings in
; %APPDATA%\AV Vibes\AV Dexed belong to the user: Setup and Uninstall never touch them.

[CustomMessages]
english.TypeFull=Full installation
spanish.TypeFull=Instalación completa
english.TypeCustom=Custom installation
spanish.TypeCustom=Instalación personalizada
english.StandaloneApp=Standalone app
spanish.StandaloneApp=App independiente (Standalone)

[Types]
Name: "full"; Description: "{cm:TypeFull}"
Name: "custom"; Description: "{cm:TypeCustom}"; Flags: iscustom
#ifdef CLAP
Name: "clap"; Description: "CLAP"
#endif
#ifdef VST3
Name: "vst3"; Description: "VST3"
#endif
#ifdef SA
Name: "standalone"; Description: "{cm:StandaloneApp}"
#endif

[Components]
#ifdef CLAP
Name: "CLAP"; Description: "CLAP"; Types: full custom clap; Flags: checkablealone
#endif

#ifdef VST3
Name: "VST3"; Description: "VST3"; Types: full custom vst3; Flags: checkablealone
#endif

#ifdef SA
Name: "SA"; Description: "{cm:StandaloneApp}"; Types: full custom standalone; Flags: checkablealone
#endif

[Files]
#ifdef CLAP
Source: "{#StagedAssets}\{#Name}.clap"; DestDir: "{autocf}\CLAP\"; Components: CLAP; Flags: ignoreversion
#endif

#ifdef VST3
Source: "{#StagedAssets}\{#Name}.vst3\*"; DestDir: "{autocf}\VST3\{#Name}.vst3\"; Components: VST3; Flags: ignoreversion recursesubdirs
#endif

#ifdef SA
Source: "{#StagedAssets}\{#Name}.exe"; DestDir: "{app}"; Components: SA; Flags: ignoreversion
#endif

[Icons]
Name: "{group}\{#Name}"; Filename: "{app}\{#Name}.exe"; Components: SA; Flags: createonlyiffileexists

#ifdef VST3
[InstallDelete]
; The VST3 is a folder: remove the old one so no file of a previous version is left in it.
Type: filesandordirs; Name: "{autocf}\VST3\{#Name}.vst3"; Components: VST3
#endif
