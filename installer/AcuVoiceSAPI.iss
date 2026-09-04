; AcuVoice SAPI5 -- Inno Setup script
;
; This is a 64-bit product with a 32-bit engine inside it, and the layout says so:
;
;   {app}                     the 64-bit SAPI5 engine, the configuration utility and the
;                             diagnostics -- everything the user runs
;   {app}\x86                 the 32-bit worker that holds the engine, and the SAPI5 dll
;                             that 32-bit hosts (NVDA, JAWS, 32-bit Balabolka) load in
;                             process. Nothing here is 32-bit by choice: avcore.dll is a
;                             1999 binary with no source and no 64-bit build, so one
;                             32-bit process has to exist to hold it.
;   {app}\engine              avcore.dll and the 154 MB recorded sound bank
;   {commonappdata}\...       the dictionary and scratch directories, writable without
;                             elevation because the engine and the dictionary editor both
;                             write to them while running as the logged-on user
;   {win}\acuvoice.ini        the only place avcore reads its directories from: it opens
;                             the file by bare name through GetPrivateProfileString, so
;                             Windows resolves it against the Windows directory and there
;                             is nowhere else it can go
;
; AcuEng.dll -- the 1999 SAPI4 engine -- is deliberately not installed. Nothing here
; needs it: avcore.dll is a plain C API with no COM and no registry.

#define AppName "AcuVoice SAPI5"
#define AppVersion "1.1.0"
#define AppPublisher "AcuVoice SAPI5 project"
#define AppURL "https://github.com/joshknnd1982/AcuVoice-sapi5"
#define SrcRoot "..\"
#define OutRoot "..\output\"

[Setup]
AppId={{7C4A1E92-3D58-4B0F-9E26-0A5F8C1D7B34}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}/releases
DefaultDirName={commonpf64}\AcuVoice SAPI5
DefaultGroupName=AcuVoice SAPI5
DisableProgramGroupPage=yes
LicenseFile={#SrcRoot}LICENSE.txt
OutputDir={#SrcRoot}output
OutputBaseFilename=AcuVoiceSAPI5_Setup_{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
; 64-bit only. The engine runs under WOW64 either way, and a product whose own binaries
; are 64-bit has no business claiming to install on 32-bit Windows.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Version 1.0.0 installed to Program Files (x86). An upgrade must not inherit that
; directory, or the 64-bit binaries would land back in the 32-bit folder.
UsePreviousAppDir=no
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\AcuVoiceConfig.exe
; Setup writes its own log next to the engine's, so a failed install and a silent voice
; can be read side by side.
SetupLogging=yes
MinVersion=6.1sp1

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Put a shortcut to the AcuVoice configuration utility on the desktop"; GroupDescription: "Shortcuts:"
Name: "setdefault"; Description: "Make AcuVoice Roger the default Windows speech voice"; GroupDescription: "Voices:"; Flags: unchecked
Name: "samples"; Description: "Install the sample wave files that show what each voice and each setting sounds like"; GroupDescription: "Extras:"

[Files]
; --- 64-bit: the product ---
Source: "{#OutRoot}AcuVoiceSAPI.dll";        DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#OutRoot}AcuVoiceConfig.exe";      DestDir: "{app}"; Flags: ignoreversion
Source: "{#OutRoot}AcuVoiceDiagnostics.exe"; DestDir: "{app}"; Flags: ignoreversion

; --- 32-bit: only what has to be ---
Source: "{#OutRoot}x86\AcuVoiceServer.exe";      DestDir: "{app}\x86"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#OutRoot}x86\AcuVoiceSAPI.dll";        DestDir: "{app}\x86"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#OutRoot}x86\AcuVoiceDiagnostics.exe"; DestDir: "{app}\x86"; Flags: ignoreversion

; --- the engine itself ---
Source: "{#SrcRoot}engine\Lib\avcore.dll";     DestDir: "{app}\engine\Lib"; Flags: ignoreversion
Source: "{#SrcRoot}engine\Ulaw08Sb\*";         DestDir: "{app}\engine\Ulaw08Sb"; Flags: ignoreversion
Source: "{#SrcRoot}engine\UserDict\*";         DestDir: "{app}\engine\UserDict"; Flags: ignoreversion

; --- the dictionary, into a directory the user can actually write to ---
Source: "{#SrcRoot}engine\Dictfls\*"; DestDir: "{commonappdata}\AcuVoice SAPI5\Dictfls"; \
    Flags: ignoreversion recursesubdirs createallsubdirs uninsneveruninstall; Permissions: users-modify

; The ini is created here rather than only by the [INI] section so that it can be given
; an ACL: set_pausation writes the pause lengths back to it and the configuration utility
; writes the custom-dictionary switch to it, and both run as the logged-on user, who
; cannot otherwise write to the Windows directory. The [INI] entries below then fill in
; the real paths.
Source: "acuvoice.ini.template"; DestDir: "{win}"; DestName: "acuvoice.ini"; \
    Flags: ignoreversion uninsneveruninstall; Permissions: users-modify

; --- documentation and samples ---
Source: "{#SrcRoot}README.md";  DestDir: "{app}"; DestName: "README.md"; Flags: ignoreversion
Source: "{#SrcRoot}LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcRoot}samples\*"; DestDir: "{app}\samples"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Tasks: samples

[Dirs]
Name: "{commonappdata}\AcuVoice SAPI5";        Permissions: users-modify
Name: "{commonappdata}\AcuVoice SAPI5\Temp";   Permissions: users-modify
Name: "{commonappdata}\AcuVoice SAPI5\Dictfls"; Permissions: users-modify

[INI]
; avcore reads nothing but this file, and it reads it every time it initializes.
Filename: "{win}\acuvoice.ini"; Section: "AcuVoiceAppDir"; Key: "SNDBANK";     String: "{app}\engine\Ulaw08Sb\"
Filename: "{win}\acuvoice.ini"; Section: "AcuVoiceAppDir"; Key: "TEMPDIR";     String: "{commonappdata}\AcuVoice SAPI5\Temp\"
Filename: "{win}\acuvoice.ini"; Section: "AcuVoiceAppDir"; Key: "DICTFLSDIR";  String: "{commonappdata}\AcuVoice SAPI5\Dictfls\"
Filename: "{win}\acuvoice.ini"; Section: "AcuVoiceAppDir"; Key: "USERDICTDIR"; String: "{app}\engine\UserDict\"
Filename: "{win}\acuvoice.ini"; Section: "AcuVoiceSettings"; Key: "PAUSE1"; String: "650"
Filename: "{win}\acuvoice.ini"; Section: "AcuVoiceSettings"; Key: "PAUSE2"; String: "500"
Filename: "{win}\acuvoice.ini"; Section: "AcuVoiceSettings"; Key: "PAUSE3"; String: "350"
Filename: "{win}\acuvoice.ini"; Section: "AcuVoiceSettings"; Key: "PAUSE4"; String: "7"
Filename: "{win}\acuvoice.ini"; Section: "AcuVoiceDictionary"; Key: "CUSTOM"; String: "NO"; Flags: createkeyifdoesntexist

[Registry]
; Both views: the 32-bit worker lookup reads the 32-bit view, the 64-bit engine the
; native one, and either may be the first thing to ask.
Root: HKLM64; Subkey: "SOFTWARE\AcuVoice SAPI5"; ValueType: string; ValueName: "InstallLocation"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM64; Subkey: "SOFTWARE\AcuVoice SAPI5"; ValueType: string; ValueName: "Version"; ValueData: "{#AppVersion}"
Root: HKLM32; Subkey: "SOFTWARE\AcuVoice SAPI5"; ValueType: string; ValueName: "InstallLocation"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM32; Subkey: "SOFTWARE\AcuVoice SAPI5"; ValueType: string; ValueName: "Version"; ValueData: "{#AppVersion}"
; The diagnostic log is on by default so a first-run problem leaves a trail. The
; configuration utility turns it off.
Root: HKCU;   Subkey: "Software\AcuVoice SAPI5"; ValueType: dword;  ValueName: "Logging"; ValueData: 1; Flags: createvalueifdoesntexist
Root: HKLM32; Subkey: "SOFTWARE\Microsoft\Speech\Voices"; ValueType: string; ValueName: "DefaultTokenId"; \
    ValueData: "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech\Voices\Tokens\AcuVoice_roger"; Tasks: setdefault
Root: HKLM64; Subkey: "SOFTWARE\Microsoft\Speech\Voices"; ValueType: string; ValueName: "DefaultTokenId"; \
    ValueData: "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech\Voices\Tokens\AcuVoice_roger"; Tasks: setdefault

[Icons]
Name: "{group}\AcuVoice Speech Configuration"; Filename: "{app}\AcuVoiceConfig.exe"; Comment: "Adjust the AcuVoice speech rate, pitch, volume and pauses"
Name: "{group}\AcuVoice Diagnostics";          Filename: "{app}\AcuVoiceDiagnostics.exe"; Comment: "Report on the AcuVoice engine and the registered voices"
Name: "{group}\AcuVoice user dictionary";      Filename: "{app}\engine\UserDict\Userdict.exe"; WorkingDir: "{app}\engine\UserDict"; Comment: "Teach AcuVoice how to pronounce a word"
Name: "{group}\AcuVoice read me";              Filename: "{app}\README.md"
Name: "{group}\Uninstall AcuVoice SAPI5";      Filename: "{uninstallexe}"
Name: "{autodesktop}\AcuVoice Speech Configuration"; Filename: "{app}\AcuVoiceConfig.exe"; Tasks: desktopicon; Comment: "Adjust the AcuVoice speech rate, pitch, volume and pauses"

[Run]
; regsvr32 is called explicitly rather than through the regserver flag so that each build
; registers through the loader of its own bitness: the 64-bit dll has to land in the
; native registry view where 64-bit SAPI looks, and the 32-bit one in WOW6432Node where
; 32-bit SAPI looks.
Filename: "{sys}\regsvr32.exe";      Parameters: "/s ""{app}\AcuVoiceSAPI.dll"""; StatusMsg: "Registering the 64-bit AcuVoice voices..."; Flags: runhidden waituntilterminated
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s ""{app}\x86\AcuVoiceSAPI.dll"""; StatusMsg: "Registering the 32-bit AcuVoice voices..."; Flags: runhidden waituntilterminated
; A self-test that runs while setup is still on screen, so a broken install says so here
; rather than as a silent screen reader later. It speaks every registered voice into a
; wave file and writes its report to %LOCALAPPDATA%\AcuVoice SAPI5\install-check.txt.
; Run as the 64-bit build, which also proves the worker starts and answers.
Filename: "{app}\AcuVoiceDiagnostics.exe"; Parameters: "selftest"; \
    StatusMsg: "Checking that every voice speaks..."; Flags: runhidden waituntilterminated
Filename: "{app}\AcuVoiceDiagnostics.exe"; Parameters: "say ""AcuVoice is installed and ready."""; \
    Description: "Say a test sentence out loud"; Flags: postinstall nowait skipifsilent
Filename: "{app}\AcuVoiceConfig.exe"; Description: "Open the AcuVoice configuration utility"; Flags: postinstall nowait skipifsilent unchecked
Filename: "{app}\README.md"; Description: "Read what AcuVoice is and what it can do"; Flags: postinstall shellexec nowait skipifsilent unchecked

[UninstallRun]
; Unregister before anything is deleted, and ask the worker to exit so its copy of
; avcore.dll stops holding the install directory open.
Filename: "{sys}\regsvr32.exe";      Parameters: "/s /u ""{app}\AcuVoiceSAPI.dll"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregX64"
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s /u ""{app}\x86\AcuVoiceSAPI.dll"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregX86"
Filename: "{sys}\taskkill.exe"; Parameters: "/f /im AcuVoiceServer.exe"; Flags: runhidden waituntilterminated; RunOnceId: "StopWorker"

[UninstallDelete]
Type: filesandordirs; Name: "{commonappdata}\AcuVoice SAPI5\Temp"
Type: filesandordirs; Name: "{app}\samples"

[Code]
const
  IniFile = 'acuvoice.ini';

// The engine keeps a machine-wide count of open synthesis channels in a shared section
// of avcore.dll, and a running worker holds both that and the file. Stopping it before
// anything is copied is what keeps an upgrade from failing on a locked dll.
procedure StopWorker;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im AcuVoiceServer.exe', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function PreviousUninstaller(): String;
var
  Key: String;
begin
  Result := '';
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\' +
         '{7C4A1E92-3D58-4B0F-9E26-0A5F8C1D7B34}_is1';
  if not RegQueryStringValue(HKLM64, Key, 'UninstallString', Result) then
    if not RegQueryStringValue(HKLM32, Key, 'UninstallString', Result) then
      Result := '';
  Result := RemoveQuotes(Result);
end;

function PreviousInstallDir(): String;
var
  Key: String;
begin
  Result := '';
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\' +
         '{7C4A1E92-3D58-4B0F-9E26-0A5F8C1D7B34}_is1';
  if not RegQueryStringValue(HKLM64, Key, 'InstallLocation', Result) then
    RegQueryStringValue(HKLM32, Key, 'InstallLocation', Result);
end;

function InitializeSetup(): Boolean;
begin
  StopWorker;
  Result := True;
end;

function InitializeUninstall(): Boolean;
begin
  StopWorker;
  Result := True;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Uninstaller, PreviousDir, WantedDir: String;
  ResultCode: Integer;
begin
  Result := '';

  // Version 1.0.0 put everything in Program Files (x86), because the whole product was
  // 32-bit then. This one is 64-bit and installs to Program Files, so an upgrade has to
  // take the old copy out first -- otherwise its voice tokens keep pointing at binaries
  // in a directory nothing writes to any more.
  PreviousDir := PreviousInstallDir();
  WantedDir := ExpandConstant('{app}');
  if (PreviousDir <> '') and (CompareText(PreviousDir, WantedDir) <> 0) then
  begin
    Uninstaller := PreviousUninstaller();
    if (Uninstaller <> '') and FileExists(Uninstaller) then
    begin
      Exec(Uninstaller, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '',
           SW_HIDE, ewWaitUntilTerminated, ResultCode);
      StopWorker;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Ini, Backup: String;
begin
  if CurStep = ssInstall then
  begin
    // A machine that already has the 1999 AcuVoice product installed has its own
    // acuvoice.ini pointing at that install. Ours replaces it, so the original is kept
    // beside it and can be put back by hand.
    Ini := ExpandConstant('{win}\' + IniFile);
    Backup := ExpandConstant('{win}\acuvoice.ini.before-AcuVoiceSAPI5');
    if FileExists(Ini) and (not FileExists(Backup)) then
      FileCopy(Ini, Backup, True);
  end;
end;
