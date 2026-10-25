; Setup for DJAP, the DJ Audio Processor (Windows 10 / 11, x64; called AudioProcessor up to 0.1.57), built with
; Inno Setup 6: tools\make_installer.ps1.
; The programs are linked with the static runtime (/MT): no Visual C++ redistributable is needed. Everything else
; they load is part of Windows; setup checks that it is there. Media Foundation (DJAPCmd reads MP3 / M4A /
; WMA / FLAC through it) is missing on the N editions of Windows: setup offers to add the Media Feature Pack.
; VAC (Virtual Audio Cable, a separate program) is needed only to run Windows' own sound through the processor:
; setup looks for it and points to its download page when it is not there.

#ifndef BuildDir
  #define BuildDir "..\build"
#endif
#define AppExe BuildDir + "\DJAP.exe"
#if !FileExists(AppExe)
  #error DJAP.exe not found in BuildDir: run build.cmd first
#endif
#define AppVersion GetStringFileInfo(AppExe, "ProductVersion")
#define AppNumVersion GetVersionNumbersString(AppExe)
; the setup's identity (AudioProcessor's, so DJAP updates it); tests compile with /DAppGuid=<another> so they never
; meet the real installation
#ifndef AppGuid
  #define AppGuid "4E31023D-F7DA-4855-AD11-F0ADCB3094CB"
#endif
#define VacPage "https://vac.muzychenko.net/en/download.htm"
; the Run value of "Start DJAP when Windows starts" (the program's menu item uses the same name); tests compile with
; /DRunValue=<another> so they never change the user's own entry
#ifndef RunValue
  #define RunValue "DJAP"
#endif
; the program's single-instance mutexes (also the old name's); tests compile with /DAppMutexes=<another> so a running
; DJAP of the user does not stop them
#ifndef AppMutexes
  #define AppMutexes "DJAP.Running,AudioProcessor.Running"
#endif

[Setup]
AppId={{{#AppGuid}}
AppName=DJAP
AppVersion={#AppVersion}
AppVerName=DJAP (DJ Audio Processor) {#AppVersion}
DefaultDirName={autopf}\DJAP
; the same AppId as AudioProcessor's setup: an update; its old folder is replaced by the new one (see [Code])
UsePreviousAppDir=no
DefaultGroupName=DJAP
DisableProgramGroupPage=yes
OutputDir=..\release
OutputBaseFilename=DJAP-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; admin for Program Files and the Media Feature Pack; /CURRENTUSER on the command line installs for one user
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
CloseApplications=yes
RestartApplications=no
; the program keeps running in the notification area: setup and uninstall ask to end it first (its menu: Exit)
AppMutex={#AppMutexes}
SetupIconFile=..\assets\DJAP.ico
WizardSmallImageFile=wizard-small-55.bmp,wizard-small-64.bmp,wizard-small-83.bmp,wizard-small-92.bmp,wizard-small-110.bmp
UninstallDisplayName=DJAP (DJ Audio Processor)
UninstallDisplayIcon={app}\DJAP.exe
VersionInfoVersion={#AppNumVersion}
VersionInfoProductName=DJAP
VersionInfoDescription=DJAP setup
SetupLogging=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
VacTitle=VAC (Virtual Audio Cable)
VacSubtitle=Needed only to run Windows' own sound through DJAP.
VacText=VAC was not found on this PC.%n%nYou need it (the free Lite version is enough) to send ordinary Windows sound - browser, music player, video - through DJAP with one channel: make a VAC line the Windows default playback device, choose that line as deck A's input in the Audio window and your speakers as the output.%n%nDJ use with two decks through an ASIO audio interface does not need it.%n%nVAC is a separate program by another author and setup does not install it. Leave "Open the VAC download page" ticked on the last page to get it.
MediaTitle=Media Foundation
MediaSubtitle=A part of Windows that this edition does not have.
MediaText=This Windows edition (an N edition) has no Media Foundation. DJAP itself runs without it, but the command-line tool DJAPCmd needs it to start and to read MP3, M4A, WMA and FLAC files.%n%nSetup can add Microsoft's Media Feature Pack now; Windows downloads it from Windows Update, which can take a few minutes.
MediaInstall=Add the Media Feature Pack now
MediaNoAdmin=(This needs an installation with administrator rights.)
MediaBusy=Adding the Media Feature Pack from Windows Update...
MediaFailed=Windows could not add the Media Feature Pack (code %1).%n%nAdd it later in Settings > System > Optional features > Add a feature > "Media Feature Pack". DJAP itself works without it.
CoreMissing=These parts of Windows are missing:%n%n%1%n%nDJAP cannot run without them. Repair Windows (for example "sfc /scannow" in an administrator command prompt) and run setup again.
ReadyChecks=System check:
ReadyCore=Windows components: all present
ReadyMediaOk=Media Foundation: present
ReadyMediaMissing=Media Foundation: missing (only DJAPCmd needs it)
ReadyMediaAdd=Media Feature Pack: will be added from Windows Update
ReadyVacFound=VAC: found (%1)
ReadyVacMissing=VAC: not found (only needed for Windows' own sound)
ReadyAsio=ASIO drivers: %1
ReadyNoAsio=ASIO drivers: none (two decks need an ASIO interface; one channel also works with WASAPI)
OpenVacPage=Open the VAC download page
DeleteSettings=Also delete your DJAP settings and own presets?%n%n%1
StartupGroup=When Windows starts:
StartupTask=Start DJAP in the notification area, with the audio devices used last

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "startup"; Description: "{cm:StartupTask}"; GroupDescription: "{cm:StartupGroup}"

[Files]
; never the developer's DJAP.ini from the build folder: settings live in %APPDATA%\DJAP
Source: "{#BuildDir}\DJAP.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\DJAPCmd.exe"; DestDir: "{app}"; Flags: ignoreversion
; no .pdb symbol files: the setup is public and they hold the build machine's paths; make_installer.ps1 keeps them
; next to the setup in release\ for --diag reports
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY.md"; DestDir: "{app}"; Flags: ignoreversion
; the manuals (tools\make_manuals.py)
Source: "..\docs\DJAP-Manual.pdf"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\DJAP-Technical-Manual.pdf"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; the shortcuts of the program's old name
Type: files; Name: "{autoprograms}\AudioProcessor.lnk"
Type: files; Name: "{autodesktop}\AudioProcessor.lnk"
; symbol files of an older installation: they would not match the new programs
Type: files; Name: "{app}\DJAP.pdb"
Type: files; Name: "{app}\DJAPCmd.pdb"

[Icons]
Name: "{autoprograms}\DJAP"; Filename: "{app}\DJAP.exe"
Name: "{autoprograms}\DJAP manual"; Filename: "{app}\DJAP-Manual.pdf"
Name: "{autoprograms}\DJAP technical manual"; Filename: "{app}\DJAP-Technical-Manual.pdf"
Name: "{autodesktop}\DJAP"; Filename: "{app}\DJAP.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\DJAP.exe"; Description: "{cm:LaunchProgram,DJAP}"; Flags: nowait postinstall skipifsilent
; an update started by the program itself (/UPDATE=1, silent): the program comes back after it, as the user
Filename: "{app}\DJAP.exe"; Parameters: "{code:RestartParams}"; Flags: nowait runasoriginaluser; Check: IsUpdateRun
Filename: "{#VacPage}"; Description: "{cm:OpenVacPage}"; Flags: shellexec nowait postinstall skipifsilent; Check: not VacFound

[Code]
const
  // imported by the programs, or loaded by the audio code (MME, DirectSound, WASAPI thread priority)
  CoreDlls = 'kernel32.dll,user32.dll,gdi32.dll,comctl32.dll,shell32.dll,ole32.dll,advapi32.dll,gdiplus.dll,dbghelp.dll,winmm.dll,dsound.dll,avrt.dll';
  MediaDlls = 'mfplat.dll,mfreadwrite.dll';
  MediaCapability = 'Media.MediaFeaturePack~~~~0.0.1.0';
  // endpoint property: the name of the device the endpoint belongs to ("Virtual Audio Cable" for VAC)
  DeviceNameValue = '{b3f8fa53-0004-438e-9003-51a46e139bfc},6';

var
  VacPresent, MediaPresent, RestartWanted: Boolean;
  VacName: String;
  VacPage: TOutputMsgWizardPage;
  MediaPage: TInputOptionWizardPage;

// The DLLs of a comma-separated list that are not in the system folder, one per line.
function MissingDlls(List: String): String;
var
  Name: String;
  P: Integer;
begin
  Result := '';
  List := List + ',';
  while List <> '' do begin
    P := Pos(',', List);
    Name := Copy(List, 1, P - 1);
    Delete(List, 1, P);
    if (Name <> '') and not FileExists(ExpandConstant('{sys}\') + Name) then
      Result := Result + Name + #13#10;
  end;
end;

function UninstallEntryFound(Root: Integer; Text: String): Boolean;
var
  Keys: TArrayOfString;
  I: Integer;
  Name: String;
begin
  Result := False;
  if RegGetSubkeyNames(Root, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', Keys) then
    for I := 0 to GetArrayLength(Keys) - 1 do
      if RegQueryStringValue(Root, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\' + Keys[I], 'DisplayName', Name)
         and (Pos(Text, Name) > 0) then begin
        Result := True;
        Exit;
      end;
end;

// A present (active or disabled) playback or recording endpoint of a device whose name contains Text.
function EndpointFound(Text: String): Boolean;
var
  Flows, Keys: TArrayOfString;
  F, I: Integer;
  Base, Name: String;
  State: Cardinal;
begin
  Result := False;
  SetArrayLength(Flows, 2);
  Flows[0] := 'Render';
  Flows[1] := 'Capture';
  for F := 0 to 1 do begin
    Base := 'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\' + Flows[F];
    if RegGetSubkeyNames(HKLM64, Base, Keys) then
      for I := 0 to GetArrayLength(Keys) - 1 do
        if RegQueryDWordValue(HKLM64, Base + '\' + Keys[I], 'DeviceState', State)
           and (((State and $F) = 1) or ((State and $F) = 2))
           and RegQueryStringValue(HKLM64, Base + '\' + Keys[I] + '\Properties', DeviceNameValue, Name)
           and (Pos(Text, Name) > 0) then begin
          Result := True;
          Exit;
        end;
  end;
end;

// The name of an installed sound driver by VAC's author: VAC itself, or a VAC built into another product under
// its own name (for example by a broadcast audio processor). '' when there is none.
function VacDriverName: String;
var
  Keys: TArrayOfString;
  I: Integer;
  Base, Provider: String;
begin
  Result := '';
  Base := 'SYSTEM\CurrentControlSet\Control\Class\{4d36e96c-e325-11ce-bfc1-08002be10318}';
  if RegGetSubkeyNames(HKLM64, Base, Keys) then
    for I := 0 to GetArrayLength(Keys) - 1 do
      if RegQueryStringValue(HKLM64, Base + '\' + Keys[I], 'ProviderName', Provider)
         and (Pos('Muzychenko', Provider) > 0)
         and RegQueryStringValue(HKLM64, Base + '\' + Keys[I], 'DriverDesc', Result) then
        Exit;
  Result := '';
end;

function VacFound: Boolean;
begin
  Result := VacPresent;
end;

// VAC is there when its driver file, its uninstall entry, one of its endpoints or a driver by its author is found.
function FindVac: Boolean;
begin
  VacName := VacDriverName;
  Result := (VacName <> '') or FileExists(ExpandConstant('{sys}\drivers\vrtaucbl.sys'))
    or UninstallEntryFound(HKLM64, 'Virtual Audio Cable') or UninstallEntryFound(HKLM32, 'Virtual Audio Cable')
    or UninstallEntryFound(HKCU, 'Virtual Audio Cable') or EndpointFound('Virtual Audio Cable');
  if VacName = '' then VacName := 'Virtual Audio Cable';
end;

// The installed ASIO drivers, comma-separated ('' when none).
function AsioDrivers: String;
var
  Keys: TArrayOfString;
  I: Integer;
begin
  Result := '';
  if RegGetSubkeyNames(HKLM64, 'SOFTWARE\ASIO', Keys) then
    for I := 0 to GetArrayLength(Keys) - 1 do begin
      if Result <> '' then Result := Result + ', ';
      Result := Result + Keys[I];
    end;
end;

// Started by the program's Install update (DJAP passes /UPDATE=1 with /SILENT).
function IsUpdateRun: Boolean;
begin
  Result := ExpandConstant('{param:UPDATE|0}') = '1';
end;

// The program's options after an update: a test run (--ini <file>) passes its settings file as /RESTARTINI=<file>.
function RestartParams(Param: String): String;
begin
  Result := ExpandConstant('{param:RESTARTINI|}');
  if Result <> '' then Result := '--ini "' + Result + '"';
end;

function InitializeSetup: Boolean;
var
  Missing: String;
  I: Integer;
begin
  // an update from the program: it ends right after starting this setup - wait for it (at most 30 s)
  if IsUpdateRun then begin
    for I := 1 to 60 do begin
      if not CheckForMutexes('{#AppMutexes}') then Break;
      Sleep(500);
    end;
    Log(Format('Update run: the program has ended: %d', [Ord(not CheckForMutexes('{#AppMutexes}'))]));
  end;
  Missing := MissingDlls(CoreDlls);
  if Missing <> '' then begin
    SuppressibleMsgBox(FmtMessage(CustomMessage('CoreMissing'), [Missing]), mbCriticalError, MB_OK, IDOK);
    Result := False;
    Exit;
  end;
  MediaPresent := MissingDlls(MediaDlls) = '';
  VacPresent := FindVac;
  // (a [Code] line must not start with "[": the compiler takes it for a section)
  Log(Format('System check: Windows components present, Media Foundation %d, VAC %d (%s), ASIO drivers "%s"', [
    Ord(MediaPresent), Ord(VacPresent), VacName, AsioDrivers]));
  Result := True;
end;

procedure InitializeWizard;
var
  Text: String;
begin
  VacPage := CreateOutputMsgPage(wpSelectTasks, CustomMessage('VacTitle'), CustomMessage('VacSubtitle'),
    CustomMessage('VacText'));
  Text := CustomMessage('MediaText');
  if not IsAdminInstallMode then Text := Text + #13#10#13#10 + CustomMessage('MediaNoAdmin');
  MediaPage := CreateInputOptionPage(VacPage.ID, CustomMessage('MediaTitle'), CustomMessage('MediaSubtitle'),
    Text, False, False);
  MediaPage.Add(CustomMessage('MediaInstall'));
  MediaPage.Values[0] := IsAdminInstallMode;
  MediaPage.CheckListBox.ItemEnabled[0] := IsAdminInstallMode;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := ((PageID = VacPage.ID) and VacPresent) or ((PageID = MediaPage.ID) and MediaPresent);
end;

function AddMedia: Boolean;
begin
  Result := not MediaPresent and IsAdminInstallMode and MediaPage.Values[0] and not IsUpdateRun;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
var
  Asio: String;
begin
  Result := MemoDirInfo + NewLine + NewLine;
  if MemoTasksInfo <> '' then Result := Result + MemoTasksInfo + NewLine + NewLine;
  Result := Result + CustomMessage('ReadyChecks') + NewLine + Space + CustomMessage('ReadyCore') + NewLine;
  if MediaPresent then
    Result := Result + Space + CustomMessage('ReadyMediaOk') + NewLine
  else begin
    Result := Result + Space + CustomMessage('ReadyMediaMissing') + NewLine;
    if AddMedia then Result := Result + Space + CustomMessage('ReadyMediaAdd') + NewLine;
  end;
  if VacPresent then
    Result := Result + Space + FmtMessage(CustomMessage('ReadyVacFound'), [VacName]) + NewLine
  else
    Result := Result + Space + CustomMessage('ReadyVacMissing') + NewLine;
  Asio := AsioDrivers;
  if Asio <> '' then
    Result := Result + Space + FmtMessage(CustomMessage('ReadyAsio'), [Asio])
  else
    Result := Result + Space + CustomMessage('ReadyNoAsio');
end;

procedure InstallMediaPack;
var
  Code: Integer;
begin
  WizardForm.StatusLabel.Caption := CustomMessage('MediaBusy');
  WizardForm.ProgressGauge.Style := npbstMarquee;
  try
    if not Exec(ExpandConstant('{sys}\dism.exe'), '/online /add-capability /capabilityname:' + MediaCapability +
                ' /norestart /quiet', '', SW_HIDE, ewWaitUntilTerminated, Code) then
      Code := -1;
  finally
    WizardForm.ProgressGauge.Style := npbstNormal;
  end;
  if Code = 3010 then
    RestartWanted := True
  else if Code <> 0 then
    SuppressibleMsgBox(FmtMessage(CustomMessage('MediaFailed'), [IntToStr(Code)]), mbError, MB_OK, IDOK);
end;

// The folder of the installation this setup updates (the same AppId, the same kind), '' when none.
function PreviousDir: String;
var
  Key: String;
begin
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#AppGuid}}_is1'; // AppId
  // only an installation of the same kind: a per-user setup never touches the one in Program Files
  if IsAdminInstallMode then begin
    if not RegQueryStringValue(HKLM64, Key, 'InstallLocation', Result) then Result := '';
  end else if not RegQueryStringValue(HKCU, Key, 'InstallLocation', Result) then
    Result := '';
end;

// Up to 0.1.57 the program was called AudioProcessor, in its own folder: its files go, the folder when empty.
procedure RemoveOldProgram;
var
  Dir: String;
begin
  Dir := RemoveBackslashUnlessRoot(PreviousDir);
  if (Dir = '') or (CompareText(Dir, RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) = 0) or
     not FileExists(Dir + '\AudioProcessor.exe') then Exit;
  Log('Removing the old AudioProcessor files from ' + Dir);
  DeleteFile(Dir + '\AudioProcessor.exe');
  DeleteFile(Dir + '\AudioProcessorCmd.exe');
  DeleteFile(Dir + '\AudioProcessor.pdb');
  DeleteFile(Dir + '\AudioProcessorCmd.pdb');
  DeleteFile(Dir + '\LICENSE.txt');
  DeleteFile(Dir + '\README.md');
  DeleteFile(Dir + '\THIRD_PARTY.md');
  DeleteFile(Dir + '\unins000.exe');
  DeleteFile(Dir + '\unins000.dat');
  DeleteFile(Dir + '\unins000.msg');
  RemoveDir(Dir);
end;

const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';

// Does the user's Run value start the DJAP of this installation folder?
function RunValueIsOurs: Boolean;
var
  Cmd: String;
begin
  Result := RegQueryStringValue(HKCU, RunKey, '{#RunValue}', Cmd) and
            (Pos(Lowercase(ExpandConstant('{app}\DJAP.exe')), Lowercase(Cmd)) > 0);
end;

// "Start DJAP when Windows starts": the same value the program's menu item writes (Audio -> Start with Windows).
// Unticked, only an entry of this installation is removed. In an admin install HKCU is the installing user's.
procedure SetStartWithWindows;
begin
  // an update from the program keeps what the user set there (the task list is the first installation's)
  if IsUpdateRun then Exit;
  if WizardIsTaskSelected('startup') then begin
    if not RegWriteStringValue(HKCU, RunKey, '{#RunValue}', '"' + ExpandConstant('{app}\DJAP.exe') + '" --autostart') then
      Log('Start with Windows: the Run value could not be written');
  end else if RunValueIsOurs then
    RegDeleteValue(HKCU, RunKey, '{#RunValue}');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then RemoveOldProgram;
  if CurStep = ssPostInstall then begin
    SetStartWithWindows;
    if AddMedia then InstallMediaPack;
  end;
end;

function NeedRestart: Boolean;
begin
  Result := RestartWanted;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Dir: String;
begin
  // the start with Windows goes with the program (also when the program's menu item made it)
  if (CurUninstallStep = usUninstall) and RunValueIsOurs then RegDeleteValue(HKCU, RunKey, '{#RunValue}');
  // settings and own presets stay unless the user says otherwise (never in a silent uninstall)
  if (CurUninstallStep = usPostUninstall) and not UninstallSilent then begin
    Dir := ExpandConstant('{userappdata}\DJAP');
    if DirExists(Dir) and (MsgBox(FmtMessage(CustomMessage('DeleteSettings'), [Dir]), mbConfirmation,
                                  MB_YESNO or MB_DEFBUTTON2) = IDYES) then
      DelTree(Dir, True, True, True);
  end;
end;
