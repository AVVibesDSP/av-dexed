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
; The first step chooses English or Spanish, with English selected.
ShowLanguageDialog=yes
LanguageDetectionMethod=none
#ifdef License
LicenseFile={#License}
#endif
#ifdef Icon
SetupIconFile={#Icon}
#endif
SetupLogging=yes
SolidCompression=yes
Compression=zip
#ifdef SA
; Explorer learns the file types of [Registry] at once.
ChangesAssociations=yes
#endif
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

#ifdef SA
[Registry]
; The file types of the Mac app (CFBundleDocumentTypes in source/CMakeLists.txt):
; a double-click on a sound opens it in the app, which imports it like a drop.
; A .syx is anybody's SysEx: the app is only offered under "Open with".
Root: HKA; Subkey: "Software\Classes\AVVibes.AVDexed.Sound"; ValueType: string; ValueData: "AV Dexed sound"; Components: SA; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\AVVibes.AVDexed.Sound\DefaultIcon"; ValueType: string; ValueData: "{app}\{#Name}.exe,0"; Components: SA
Root: HKA; Subkey: "Software\Classes\AVVibes.AVDexed.Sound\shell\open\command"; ValueType: string; ValueData: """{app}\{#Name}.exe"" ""%1"""; Components: SA
Root: HKA; Subkey: "Software\Classes\AVVibes.AVDexed.SysExBank"; ValueType: string; ValueData: "DX7 SysEx bank"; Components: SA; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\AVVibes.AVDexed.SysExBank\DefaultIcon"; ValueType: string; ValueData: "{app}\{#Name}.exe,0"; Components: SA
Root: HKA; Subkey: "Software\Classes\AVVibes.AVDexed.SysExBank\shell\open\command"; ValueType: string; ValueData: """{app}\{#Name}.exe"" ""%1"""; Components: SA
Root: HKA; Subkey: "Software\Classes\.avdexpreset"; ValueType: string; ValueData: "AVVibes.AVDexed.Sound"; Components: SA; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.avdexpreset\OpenWithProgids"; ValueType: string; ValueName: "AVVibes.AVDexed.Sound"; ValueData: ""; Components: SA; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.dexedpreset"; ValueType: string; ValueData: "AVVibes.AVDexed.Sound"; Components: SA; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.dexedpreset\OpenWithProgids"; ValueType: string; ValueName: "AVVibes.AVDexed.Sound"; ValueData: ""; Components: SA; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.syx"; Components: SA; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.syx\OpenWithProgids"; ValueType: string; ValueName: "AVVibes.AVDexed.SysExBank"; ValueData: ""; Components: SA; Flags: uninsdeletevalue uninsdeletekeyifempty
#endif

#ifdef VST3
[InstallDelete]
; The VST3 is a folder: remove the old one so no file of a previous version is left in it.
Type: filesandordirs; Name: "{autocf}\VST3\{#Name}.vst3"; Components: VST3
#endif
