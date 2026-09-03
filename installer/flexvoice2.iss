; FlexVoice 2 SAPI5 installer.
;
; Registration writes to HKLM, because SAPI reads voice tokens from there and
; nowhere else, so this needs administrator rights.
;
; Two things here are unusual and both are deliberate:
;
;   1. Two of the engine's data files are BUILT during installation.
;      FlexVoice 2.0 never shipped RHL2.dat or Julie.bin; Mindmaker's own setup
;      generated them from CHL2.dat and Julie.cod with FVZip.exe, and without
;      them the engine faults on a null pointer inside createEngine rather than
;      reporting anything. Generating them saves about four megabytes, and the
;      self-test below refuses to let a broken install pass quietly.
;
;   2. The registration is removed by THREE independent mechanisms, none of
;      which can take the others down with it:
;        a. DllUnregisterServer, via the regserver flag on the DLLs.
;        b. [Registry] entries flagged uninsdeletekey dontcreatekey, recorded in
;           unins000.dat. Inno removes these itself; they do not care whether
;           regsvr32 ran or whether the DLL is still loadable.
;        c. CurUninstallStepChanged in [Code], which sweeps by name prefix in
;           both registry views.
;      FlexVoice 3.01 had only (a), and (a) did not work: RegDeleteKeyW cannot
;      delete a key that has subkeys, every SAPI token has an Attributes subkey,
;      and the failure was swallowed. Uninstalling left nine voices behind
;      naming a CLSID that no longer resolved, and because NVDA remembers its
;      voice by token path, that stopped SAPI5 working entirely -- for every
;      vendor's voices, not just ours.

#define MyAppName "FlexVoice 2 SAPI5"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Josh Kennedy"
#define MyAppURL "https://github.com/joshknnd1982/flexxVoice-v2-sapi5"

#define SpeechTokens "Software\Microsoft\Speech\Voices\Tokens"
#define ClsidRoot    "Software\Classes\CLSID"
#define EngineClsid  "{{B7E42D61-3C95-4A18-9F6B-2ED40C8A5713}"

[Setup]
AppId={{9C4A1E77-2B58-4E63-B0D1-5F7A3C8E9D24}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={autopf}\FlexVoice2SAPI
DefaultGroupName=FlexVoice 2 SAPI5
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
; The version is in the filename as well as in every binary's resources, so two
; downloads sitting in the same folder can be told apart at a glance.
OutputBaseFilename=FlexVoice2SAPI_Setup_{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayIcon={app}\FlexVoice2Config.exe
VersionInfoVersion={#MyAppVersion}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoDescription={#MyAppName} Setup
VersionInfoCompany={#MyAppPublisher}
VersionInfoCopyright=Copyright (c) 2026 Josh Kennedy. FlexVoice 2.0 engine (c) 2001 Mindmaker Ltd.
; The install log is a real debugging aid for a speech engine that will not
; speak, so keep it and copy it somewhere the user can find.
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon for the FlexVoice 2 configuration utility"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; The 32-bit COM server, registered into the 32-bit view.
Source: "..\output\FlexVoice2SAPI.dll";     DestDir: "{app}";     Flags: ignoreversion regserver 32bit
; The 64-bit COM server, registered into the native view.
Source: "..\output\x64\FlexVoice2SAPI.dll"; DestDir: "{app}\x64"; Flags: ignoreversion regserver 64bit; Check: Is64BitInstallMode

Source: "..\output\fv2_host.exe";           DestDir: "{app}"; Flags: ignoreversion
Source: "..\output\FlexVoice2Config.exe";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\output\FlexVoice_2_00_010.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\output\engine\*";               DestDir: "{app}\engine"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\output\README.md";              DestDir: "{app}"; Flags: ignoreversion
Source: "..\output\LICENSE";                DestDir: "{app}"; Flags: ignoreversion
Source: "..\output\CREDITS.md";             DestDir: "{app}"; Flags: ignoreversion

[Registry]
; Mechanism (b). `dontcreatekey` means Setup writes nothing at install time --
; DllRegisterServer still owns creating these -- while `uninsdeletekey` records
; the key in unins000.dat so the uninstaller deletes it, and its subkeys, on its
; own. That is what makes removal independent of regsvr32.
;
; The 32-bit DLL registers into the WOW6432Node view and the 64-bit DLL into the
; native one, so both views are listed. Anything not named here -- a voice
; renamed in a later release -- is caught by the prefix sweep in [Code].
Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice2_CustomVoice"; Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice2_Julie";       Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice2_Bill";        Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice2_Jill";        Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice2_Julius";      Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#SpeechTokens}\FlexVoice2_Kit";         Flags: uninsdeletekey dontcreatekey
Root: HKLM32; Subkey: "{#ClsidRoot}\{#EngineClsid}";            Flags: uninsdeletekey dontcreatekey

Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice2_CustomVoice"; Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice2_Julie";       Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice2_Bill";        Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice2_Jill";        Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice2_Julius";      Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#SpeechTokens}\FlexVoice2_Kit";         Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode
Root: HKLM64; Subkey: "{#ClsidRoot}\{#EngineClsid}";            Flags: uninsdeletekey dontcreatekey; Check: Is64BitInstallMode

[Icons]
Name: "{group}\FlexVoice 2 Configuration";       Filename: "{app}\FlexVoice2Config.exe"
Name: "{group}\Uninstall FlexVoice 2 SAPI5";     Filename: "{uninstallexe}"
Name: "{autodesktop}\FlexVoice 2 Configuration"; Filename: "{app}\FlexVoice2Config.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\FlexVoice2Config.exe"; Description: "Open the FlexVoice 2 configuration utility"; Flags: postinstall nowait skipifsilent unchecked

[UninstallRun]
; Stop the engine host so its files can be removed.
Filename: "{app}\fv2_host.exe"; Parameters: "--shutdown"; Flags: runhidden waituntilterminated; RunOnceId: "StopFlexVoice2Host"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\logs"
; The two files built during installation are not in unins000.dat, because
; Setup never copied them. Without these the program folder is left behind.
Type: files; Name: "{app}\engine\Data\RHL2.dat"
Type: files; Name: "{app}\engine\Data\Julie.bin"
Type: dirifempty; Name: "{app}\engine\Data\Voices"
Type: dirifempty; Name: "{app}\engine\Data"
Type: dirifempty; Name: "{app}\engine"

[Code]

const
  TokenPrefix = 'FlexVoice2_';

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

// Every FlexVoice 2 voice token in one view, found by name rather than from a
// list this installer carries, so a voice renamed or dropped in some later
// release is removed too. Only keys under Speech\Voices\Tokens whose name
// starts with FlexVoice2_ ever match, so no other vendor's voice can be.
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
// Every other user hive that happens to be loaded is checked as well, because a
// default voice is per user and the one who installed FlexVoice 2 need not be
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

// Everything FlexVoice 2 has ever written outside its own program folder.
procedure RemoveAllRegistration();
var
  V: Integer;
begin
  RepairDefaultVoice();
  for V := 0 to ViewCount() - 1 do
  begin
    SweepVoiceTokens(ViewRoot(V));
    DropKey(ViewRoot(V), 'Software\Classes\CLSID\{B7E42D61-3C95-4A18-9F6B-2ED40C8A5713}');
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
  HostExe: String;
begin
  Result := '';
  // A host left over from a previous install holds FlexVoice_2_00_010.dll open.
  // Stop it before replacing files.
  HostExe := ExpandConstant('{app}\fv2_host.exe');
  if FileExists(HostExe) then
    Exec(HostExe, '--shutdown', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

// Build the two data files FlexVoice 2.0 never shipped. The braces really are
// part of FVZip's command line -- they are not Inno syntax and not decoration.
// FVZip exits 6 and writes nothing if they are left out.
function BuildEngineData(): String;
var
  DataDir, Zip: String;
  Code: Integer;
begin
  Result := '';
  DataDir := ExpandConstant('{app}\engine\Data');
  Zip := DataDir + '\FVZip.exe';

  if not FileExists(Zip) then
  begin
    Result := 'FVZip.exe is missing from the engine data folder, so the ' +
              'letter-to-sound table and the diphone database cannot be built.';
    Exit;
  end;

  if not FileExists(DataDir + '\RHL2.dat') then
  begin
    Log('Building RHL2.dat from CHL2.dat');
    Exec(Zip, 'chl2rhl { CHL2.dat RHL2.dat }', DataDir, SW_HIDE,
         ewWaitUntilTerminated, Code);
  end;
  if not FileExists(DataDir + '\Julie.bin') then
  begin
    Log('Building Julie.bin from Julie.cod');
    Exec(Zip, 'vq2bin { Julie.cod Julie.bin }', DataDir, SW_HIDE,
         ewWaitUntilTerminated, Code);
  end;

  if not FileExists(DataDir + '\RHL2.dat') then
    Result := 'The letter-to-sound table (RHL2.dat) could not be built.';
  if not FileExists(DataDir + '\Julie.bin') then
    Result := Result + ' The voice database (Julie.bin) could not be built.';

  if Result <> '' then
    Result := Result + #13#10#13#10 +
              'This usually means security software blocked FVZip.exe in ' +
              ExpandConstant('{app}\engine\Data') + '. ' +
              'FlexVoice 2 will not speak until it is allowed to run.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  LogDir, Problem: String;
  Code, V: Integer;
begin
  if CurStep = ssInstall then
  begin
    // Clear any orphaned tokens before DllRegisterServer writes this release's.
    // The default-voice pointer is deliberately left alone here: the voice it
    // names is about to exist again.
    for V := 0 to ViewCount() - 1 do
      SweepVoiceTokens(ViewRoot(V));
  end;

  if CurStep = ssPostInstall then
  begin
    Problem := BuildEngineData();
    if Problem <> '' then
    begin
      Log('Engine data build failed: ' + Problem);
      MsgBox(Problem, mbError, MB_OK);
    end
    else
    begin
      // Prove it before the user finds out the hard way. The self-test loads
      // the engine and speaks with every voice; it is the same check the
      // configuration utility's status line reports.
      Exec(ExpandConstant('{app}\fv2_host.exe'), '--self-test',
           ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code);
      Log('Engine self-test exit code: ' + IntToStr(Code));
      if Code <> 0 then
        MsgBox('FlexVoice 2 installed, but its engine self-test did not pass. ' +
               'Open the configuration utility and use Speak it to see what ' +
               'happens, then send the log folder if you report this.',
               mbError, MB_OK);
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
  // on the uninstall log being intact. That is the whole point: FlexVoice 3.01
  // had one mechanism and it did not work.
  if (CurUninstallStep = usUninstall) or (CurUninstallStep = usPostUninstall) then
    RemoveAllRegistration();
end;
