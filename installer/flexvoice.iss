; FlexVoice SAPI5 installer.
;
; Registration writes to HKLM (SAPI reads voice tokens from there and nowhere
; else), so this needs administrator rights.
;
; A note on the uninstall, because it is the part that has to be right.
;
; SAPI5 is a shared, machine-wide list. Every voice on the machine, from every
; vendor, is a subkey of one registry key. An installer that leaves a broken
; entry in that key is not leaving a mess of its own -- it is handing every
; other speech engine on the machine a voice that cannot be created, and a user
; whose default voice was ours is left with a speech system that does not
; speak. 1.0.2 did exactly that: its DllUnregisterServer called RegDeleteKeyW,
; which will not delete a key that has subkeys, and every voice token has an
; Attributes subkey, so not one token was ever removed.
;
; So the uninstall now removes FlexVoice's registry footprint three times over,
; by three mechanisms that fail independently:
;
;   1. DllUnregisterServer in the DLL, via regsvr32, as before -- now with a
;      recursive delete that works.
;   2. [Registry] entries below, recorded in unins000.dat. Inno removes these
;      itself; they do not care whether regsvr32 ran, or whether the DLL is
;      still present and loadable.
;   3. CurUninstallStepChanged in [Code], which sweeps by name prefix in both
;      registry views, so a voice renamed in some earlier release is caught
;      too, and which clears a default-voice pointer aimed at a voice that is
;      about to stop existing.
;
; Nothing here ever deletes a parent key. Speech\Voices\Tokens and
; Speech\Voices\TokenEnums belong to the machine, not to FlexVoice.

#define MyAppName "FlexVoice SAPI5"
#define MyAppVersion "1.0.3"
#define MyAppPublisher "Josh Kennedy"
#define MyAppURL "https://github.com/joshknnd1982/flexVoice-sapi5"

[Setup]
AppId={{316DB6AE-DE51-41BA-BAF4-BA78FFC715DD}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={autopf}\FlexVoiceSAPI
DefaultGroupName=FlexVoice SAPI5
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
; The version is in the filename as well as the resources, so two
; downloads sitting in the same folder are told apart at a glance.
OutputBaseFilename=FlexVoiceSAPI_Setup_{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayIcon={app}\FlexVoiceConfig.exe
; Without these the setup executable carries no version resource at all, and a
; user with two copies in Downloads cannot tell which is which.
VersionInfoVersion={#MyAppVersion}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoDescription={#MyAppName} Setup
VersionInfoCompany={#MyAppPublisher}
VersionInfoCopyright=Copyright (c) 2026 Josh Kennedy. FlexVoice engine (c) Mindmaker Ltd.
; The install log is a real debugging aid for a speech engine that will not
; speak, so keep it and copy it somewhere the user can find.
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon for the FlexVoice configuration utility"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; The 32-bit COM server, registered into the 32-bit view.
Source: "..\output\FlexVoiceSAPI.dll";      DestDir: "{app}";     Flags: ignoreversion regserver 32bit
; The 64-bit COM server, registered into the native view.
Source: "..\output\x64\FlexVoiceSAPI.dll";  DestDir: "{app}\x64"; Flags: ignoreversion regserver 64bit; Check: Is64BitInstallMode

Source: "..\output\flexvoice_host.exe";     DestDir: "{app}"; Flags: ignoreversion
Source: "..\output\FlexVoiceConfig.exe";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\output\FlexVoice_3_01_001.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\output\flexvoice_diag32.exe";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\output\flexvoice_diag64.exe";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\output\engine\*";               DestDir: "{app}\engine"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\output\README.md";              DestDir: "{app}"; Flags: ignoreversion
Source: "..\output\LICENSE";                DestDir: "{app}"; Flags: ignoreversion
Source: "..\output\CREDITS.md";             DestDir: "{app}"; Flags: ignoreversion

[Registry]
; Mechanism 2. `dontcreatekey` means Setup writes nothing at install time --
; DllRegisterServer still owns creating these -- while `uninsdeletekey` records
; the key in unins000.dat so the uninstaller deletes it, and its subkeys, on its
; own. That is what makes the removal independent of regsvr32.
;
; The 32-bit DLL registers into the WOW6432Node view and the 64-bit DLL into the
; native one, so both views are listed. Anything not named here -- a voice
; renamed since an older release -- is caught by the prefix sweep in [Code].
#define SpeechTokens "Software\Microsoft\Speech\Voices\Tokens"
#define SpeechEnums  "Software\Microsoft\Speech\Voices\TokenEnums"
#define ClsidRoot    "Software\Classes\CLSID"
#define EngineClsid  "{{5E1FA20E-0311-4DBA-88A4-8455C601B75F}"
#define EnumClsid    "{{A991284C-56BE-4324-99B2-694484E0A867}"

Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice_Custom_Voice"; Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice_Julie";        Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice_Kim";          Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice_Tim";          Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice_Bill";         Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice_Julius";       Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice_Julia";        Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice_Jill";         Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice_Kit";          Flags: uninsdeletekey dontcreatekey
; Written by releases up to 1.0.2 only; removed here and never recreated.
Root: HKLM32; Subkey: "{#SpeechEnums}\FlexVoice";               Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#ClsidRoot}\{#EngineClsid}";            Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#ClsidRoot}\{#EnumClsid}";              Flags: uninsdeletekey dontcreatekey

Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice_Custom_Voice"; Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice_Julie";        Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice_Kim";          Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice_Tim";          Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice_Bill";         Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice_Julius";       Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice_Julia";        Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice_Jill";         Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice_Kit";          Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#SpeechEnums}\FlexVoice";               Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#ClsidRoot}\{#EngineClsid}";            Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#ClsidRoot}\{#EnumClsid}";              Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode

[Icons]
Name: "{group}\FlexVoice Configuration";      Filename: "{app}\FlexVoiceConfig.exe"
Name: "{group}\Uninstall FlexVoice SAPI5";    Filename: "{uninstallexe}"
Name: "{autodesktop}\FlexVoice Configuration"; Filename: "{app}\FlexVoiceConfig.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\FlexVoiceConfig.exe"; Description: "Open the FlexVoice configuration utility"; Flags: postinstall nowait skipifsilent unchecked

[UninstallRun]
; Stop the engine host so its files can be removed.
Filename: "{app}\flexvoice_host.exe"; Parameters: "--shutdown"; Flags: runhidden waituntilterminated; RunOnceId: "StopFlexVoiceHost"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\logs"

[Code]

const
  TokenPrefix = 'FlexVoice_';

// The registry views to clean. On 64-bit Windows the 32-bit DLL registers into
// WOW6432Node and the 64-bit DLL into the native view, and a leftover in either
// one is a broken voice for the clients that read that view. On 32-bit Windows
// there is only the one, and touching HKLM64 there is an error.
function ViewCount(): Integer;
begin
  if IsWin64 then Result := 2 else Result := 1;
end;

function ViewRoot(Index: Integer): Integer;
begin
  if Index = 0 then Result := HKLM32 else Result := HKLM64;
end;

// Deletes one key and everything under it, and says whether the key is gone
// afterwards. A key that was not there to begin with counts as gone.
function DropKey(Root: Integer; const Key: String): Boolean;
begin
  if not RegKeyExists(Root, Key) then
  begin
    Result := True;
    Exit;
  end;
  RegDeleteKeyIncludingSubkeys(Root, Key);
  Result := not RegKeyExists(Root, Key);
end;

// Every FlexVoice voice token in one view, found by name rather than from a
// list this installer carries, so a voice that was renamed or dropped in some
// earlier release is removed too. Only keys under Speech\Voices\Tokens whose
// name starts with FlexVoice_ ever match, so no other vendor's voice can be.
//
// Speech\Voices\Tokens itself is never touched. It is the machine's voice list.
procedure SweepVoiceTokens(Root: Integer);
var
  Names: TArrayOfString;
  I: Integer;
begin
  if not RegGetSubkeyNames(Root, 'Software\Microsoft\Speech\Voices\Tokens', Names) then
    Exit;
  for I := 0 to GetArrayLength(Names) - 1 do
  begin
    if Pos(Uppercase(TokenPrefix), Uppercase(Names[I])) = 1 then
      DropKey(Root, 'Software\Microsoft\Speech\Voices\Tokens\' + Names[I]);
  end;
end;

// A DefaultTokenId naming a voice that is about to stop existing leaves the
// user's speech pointing at nothing. SAPI picks a voice on its own when the
// value is absent, so clearing it is the safe repair -- and much safer than
// nominating a replacement, since the same string is resolved through a
// different registry view by 32-bit and 64-bit clients.
procedure ClearDefaultVoiceIfOurs(Root: Integer; const SpeechKey: String);
var
  Current: String;
begin
  if not RegQueryStringValue(Root, SpeechKey, 'DefaultTokenId', Current) then
    Exit;
  if Pos(Uppercase('\Voices\Tokens\' + TokenPrefix), Uppercase(Current)) > 0 then
    RegDeleteValue(Root, SpeechKey, 'DefaultTokenId');
end;

// The uninstaller runs elevated, so HKCU is whichever account approved it.
// Every other user hive that happens to be loaded is checked as well, because
// a default voice is per user and the one who installed FlexVoice need not be
// the one who is about to lose their speech.
procedure RepairDefaultVoice();
var
  Hives: TArrayOfString;
  I: Integer;
begin
  ClearDefaultVoiceIfOurs(HKCU, 'Software\Microsoft\Speech\Voices');
  if RegGetSubkeyNames(HKU, '', Hives) then
  begin
    for I := 0 to GetArrayLength(Hives) - 1 do
    begin
      if Pos('_CLASSES', Uppercase(Hives[I])) = 0 then
        ClearDefaultVoiceIfOurs(HKU, Hives[I] + '\Software\Microsoft\Speech\Voices');
    end;
  end;
end;

// Everything FlexVoice has ever written outside its own program folder.
procedure RemoveAllRegistration();
var
  V: Integer;
begin
  RepairDefaultVoice();
  for V := 0 to ViewCount() - 1 do
  begin
    SweepVoiceTokens(ViewRoot(V));
    // Written by releases up to 1.0.2 only. It made SAPI list every FlexVoice
    // voice twice, once from the static token and once from the enumerator.
    DropKey(ViewRoot(V), 'Software\Microsoft\Speech\Voices\TokenEnums\FlexVoice');
    DropKey(ViewRoot(V), 'Software\Classes\CLSID\{5E1FA20E-0311-4DBA-88A4-8455C601B75F}');
    DropKey(ViewRoot(V), 'Software\Classes\CLSID\{A991284C-56BE-4324-99B2-694484E0A867}');
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
  HostExe: String;
begin
  Result := '';
  // An engine host left over from a previous install holds FlexVoice_3_01_001.dll
  // open, and it speaks the old wire protocol. Stop it before replacing files.
  HostExe := ExpandConstant('{app}\flexvoice_host.exe');
  if FileExists(HostExe) then
  begin
    Exec(HostExe, '--shutdown', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  LogDir: String;
  V: Integer;
begin
  if CurStep = ssInstall then
  begin
    // Upgrading from 1.0.2 or earlier: those releases could not remove a voice
    // token, so a machine may be carrying orphans, and it is certainly carrying
    // the TokenEnums entry that made every voice appear twice. Clear both
    // before DllRegisterServer writes this release's tokens. The default-voice
    // pointer is deliberately left alone here -- the voice it names is about to
    // exist again.
    for V := 0 to ViewCount() - 1 do
    begin
      SweepVoiceTokens(ViewRoot(V));
      DropKey(ViewRoot(V), 'Software\Microsoft\Speech\Voices\TokenEnums\FlexVoice');
    end;
  end;

  if CurStep = ssDone then
  begin
    // Keep the install log next to the application; when a speech engine will
    // not speak, this is the first thing worth reading.
    LogDir := ExpandConstant('{app}\logs');
    CreateDir(LogDir);
    CopyFile(ExpandConstant('{log}'), LogDir + '\install.log', False);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  // Twice, on purpose.
  //
  // usUninstall runs before anything is deleted, so the registration goes while
  // the DLL is still there and a SAPI5 client that looks in between sees a
  // consistent machine. usPostUninstall runs after the files are gone and after
  // regsvr32 has had its turn, and catches anything that reappeared or that a
  // failed unregistration left behind.
  //
  // Neither pass depends on the DLL being loadable, on regsvr32 succeeding, or
  // on the uninstall log being intact. That is the whole point: 1.0.2 had one
  // mechanism and it did not work.
  if (CurUninstallStep = usUninstall) or (CurUninstallStep = usPostUninstall) then
    RemoveAllRegistration();
end;
