; ModAVD 安装程序（Inno Setup 6）
;
; 薄安装程序：把发版 zip 的内容原样放进用户选的目录，加开始菜单、桌面快捷方式和卸载项。
; 根目录是自包含的，以后由管理器自己整包更新；安装程序不管这些文件之后的变化。
; 不要直接用 ISCC 编，用同目录的 build-installer.ps1（它核对载荷、传下面这些定义）。
;
; 编译时要传的定义：
;   /DAppVersion=0.43.0          版本号
;   /DPayloadDir=<目录>           发版 zip 解压出来的目录（顶层就是 ModAVD.exe、modavd-manifest.json）
;   /DOutputBaseName=<名字>       输出文件名，不带 .exe
; 可选：
;   /DSelfTest                    编出只跑自检的安装程序：校验和默认目录的逻辑在日志里逐条核对，然后退出，不装任何东西
;
; 界面语言：按系统界面语言自动选，不弹选择框。中文系统（简体、繁体都算）用简体中文，其余用英文。
; 以后加一种语言：加一个 Inno 的 .isl、一份我们自己的 messages-<语言>.isl，再在 [Languages] 加一行。

#ifndef AppVersion
  #error 缺少 /DAppVersion
#endif
#ifndef SelfTest
  #ifndef PayloadDir
    #error 缺少 /DPayloadDir
  #endif
#endif
#ifndef OutputBaseName
  #define OutputBaseName "ModAVD-" + AppVersion + "-win-x64-setup"
#endif

; 安装目录最长多少个字符（不含结尾的 \）。依据见 README.md「路径长度」。
#define MaxDirLen 80

[Setup]
; AppId 固定不变：卸载项的键名就是它，改了会让旧的卸载项对不上。
AppId={{6F1D2C3B-8E4A-4B7F-9C5D-2A7E0B9F4D13}
AppName=ModAVD
AppVersion={#AppVersion}
AppVerName=ModAVD {#AppVersion}
AppPublisher=ModAVD
AppPublisherURL=https://github.com/ModAVD/ModAVD
AppSupportURL=https://github.com/ModAVD/ModAVD/issues
AppUpdatesURL=https://github.com/ModAVD/ModAVD/releases
VersionInfoVersion={#AppVersion}
VersionInfoProductName=ModAVD
VersionInfoDescription=ModAVD Setup
; 不要管理员权限：卸载项写 HKCU，快捷方式放当前用户的开始菜单和桌面。
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DefaultDirName={code:GetDefaultDir}
; 上次装的目录里已经有一份 ModAVD，会被校验拦住，所以不沿用上次的目录。
UsePreviousAppDir=no
DisableDirPage=no
DirExistsWarning=no
; 盘根由我们自己的校验拦，给出看得懂的原因（Inno 自带的拦法只说「要输入带盘符的完整路径」）。
AllowRootDirectory=yes
DisableProgramGroupPage=yes
DisableWelcomePage=yes
ShowLanguageDialog=no
LanguageDetectionMethod=uilanguage
WizardStyle=modern
; 图标原样取自 ModAVDManager 的 cmd/modavd-gui/modavd.ico（和 ModAVD.exe 的图标相同）。
SetupIconFile=modavd.ico
UninstallDisplayIcon={app}\ModAVD.exe
UninstallDisplayName=ModAVD
OutputDir=.
OutputBaseFilename={#OutputBaseName}
Compression=lzma2/max
SolidCompression=yes
LZMAUseSeparateProcess=yes
; 装完不改关联、环境变量，不需要重启，也不去关别的程序。
ChangesAssociations=no
ChangesEnvironment=no
CloseApplications=no
RestartIfNeededByRun=no

[Languages]
; 每种语言两份文件：Inno 自己的标准文字，加上我们自己的提示（messages-*.isl 的 [CustomMessages]）。
Name: "en"; MessagesFile: "compiler:Default.isl,messages-en.isl"
Name: "zh_cn"; MessagesFile: "ChineseSimplified.isl,messages-zh.isl"
; 繁体中文系统：Inno 自动选语言时，不会把简体中文分给繁体系统（见 Inno 帮助 [Languages] 一节），
; 没有这一项就落到第一项英文。这里用同样的简体中文文字，只把语言号设成繁体（台湾 $0404）：
; zh-TW 完全对上；zh-HK（$0C04）、zh-MO（$1404）主语言号相同、又同属繁体，第二步也落到这一项。
Name: "zh_tw"; MessagesFile: "ChineseSimplified.isl,messages-zh.isl"

[LangOptions]
zh_tw.LanguageID=$0404

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
#ifndef SelfTest
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
#endif

[Icons]
Name: "{autoprograms}\ModAVD"; Filename: "{app}\ModAVD.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\ModAVD"; Filename: "{app}\ModAVD.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; 只在交互安装的完成页给一个勾选项；静默安装（/SILENT、/VERYSILENT）一律不启动。
Filename: "{app}\ModAVD.exe"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,ModAVD}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 卸载时删的程序文件。安装时放进去的文件 Inno 自己会删；这里列的是程序目录本身，
; 管理器自更新以后多出来的程序文件也一起删掉。只列程序，用户数据一项都不在这里（分类见 README.md）。
Type: filesandordirs; Name: "{app}\sdk\emulator"
Type: filesandordirs; Name: "{app}\tools"
Type: filesandordirs; Name: "{app}\components"
Type: filesandordirs; Name: "{app}\extras"
Type: filesandordirs; Name: "{app}\mumu-shim"
Type: filesandordirs; Name: "{app}\LICENSES"
Type: files; Name: "{app}\ModAVD.exe"
Type: files; Name: "{app}\modavd-manifest.json"
Type: dirifempty; Name: "{app}\sdk"
Type: dirifempty; Name: "{app}"

[Code]
const
  DRIVE_FIXED = 3;
  MaxDirLen = {#MaxDirLen};

var
  FreeSpaceLabel: TNewStaticText;
  OldDirEditChange: TNotifyEvent;
  DeleteUserData: Boolean;

function GetDriveTypeW(lpRootPathName: String): Cardinal;
  external 'GetDriveTypeW@kernel32.dll stdcall';

{ ---- 默认目录 ---------------------------------------------------------------------------------- }

{ 纯函数，方便自检：有 D 盘（本地固定硬盘）就 D:\ModAVD；没有 D 有 E 就 E:\ModAVD；都没有就 C:\ModAVD。 }
function PickDefaultDir(TypeD, TypeE: Integer): String;
begin
  if TypeD = DRIVE_FIXED then
    Result := 'D:\ModAVD'
  else if TypeE = DRIVE_FIXED then
    Result := 'E:\ModAVD'
  else
    Result := 'C:\ModAVD';
end;

function GetDefaultDir(Param: String): String;
var
  TypeD, TypeE: Integer;
begin
  TypeD := GetDriveTypeW('D:\');
  TypeE := GetDriveTypeW('E:\');
  Result := PickDefaultDir(TypeD, TypeE);
  Log(Format('Default dir: %s (drive type D=%d, E=%d; 3 = fixed)', [Result, TypeD, TypeE]));
  Log(Format('Language: %s (UI language $%.4x)', [ActiveLanguage, GetUILanguage]));
end;

{ ---- 目录校验 ---------------------------------------------------------------------------------- }

function NormalizeDir(Dir: String): String;
begin
  Result := RemoveBackslash(Trim(Dir));
end;

{ Path 是 Base 本身或在它下面（不分大小写）。Base 为空时不算。 }
function IsUnder(Path, Base: String): Boolean;
var
  P, B: String;
begin
  Result := False;
  B := Lowercase(NormalizeDir(Base));
  if B = '' then
    exit;
  P := Lowercase(NormalizeDir(Path));
  Result := (P = B) or (Copy(P, 1, Length(B) + 1) = B + '\');
end;

function HasNonAscii(S: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to Length(S) do
    if (Ord(S[I]) > 126) or (Ord(S[I]) < 32) then
    begin
      Result := True;
      exit;
    end;
end;

function IsDriveLetterPath(P: String): Boolean;
var
  C: Char;
begin
  Result := False;
  if Length(P) < 2 then
    exit;
  C := Uppercase(Copy(P, 1, 1))[1];
  Result := (C >= 'A') and (C <= 'Z') and (P[2] = ':') and ((Length(P) = 2) or (P[3] = '\'));
end;

{ 系统目录、OneDrive：Dir 在其中某一个之下时返回 True，Hit 是命中的那个目录。 }
function UnderAny(Dir: String; Bases: TArrayOfString; var Hit: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 0 to GetArrayLength(Bases) - 1 do
    if IsUnder(Dir, Bases[I]) then
    begin
      Hit := NormalizeDir(Bases[I]);
      Result := True;
      exit;
    end;
end;

{ 目录不存在，或者存在但里面什么都没有。 }
function DirIsEmptyOrMissing(P: String): Boolean;
var
  FindRec: TFindRec;
begin
  Result := True;
  if not DirExists(P) then
    exit;
  if FindFirst(AddBackslash(P) + '*', FindRec) then
  try
    repeat
      if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
      begin
        Result := False;
        exit;
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
end;

{ 交互时在选目录页点「下一步」调，静默安装时在 PrepareToInstall 再调一次。合格返回 True；不合格返回 False，
  Reason 是给用户看的一句话。只读，不建目录也不写文件。 }
function ValidateInstallDir(Dir: String; var Reason: String): Boolean;
var
  P, Hit: String;
  DriveType: Cardinal;
  SystemDirs, OneDriveDirs: TArrayOfString;
begin
  Result := False;
  Reason := '';
  P := NormalizeDir(Dir);

  { 1. 本机盘符路径。\\server\share、\\?\ 这类都不算。 }
  if (P = '') or not IsDriveLetterPath(P) then
  begin
    Reason := CustomMessage('ErrNotLocal');
    exit;
  end;

  { 2. 只许 ASCII：模拟器和 Qt 对中文等字符的路径支持不全。 }
  if HasNonAscii(P) then
  begin
    Reason := CustomMessage('ErrNonAscii');
    exit;
  end;

  { 3. 盘的根目录本身不行，要一个子文件夹。 }
  if Length(P) = 2 then
  begin
    Reason := FmtMessage(CustomMessage('ErrDriveRoot'), [P + '\']);
    exit;
  end;

  { 4. 长度。 }
  if Length(P) > MaxDirLen then
  begin
    Reason := FmtMessage(CustomMessage('ErrTooLong'), [IntToStr(MaxDirLen), IntToStr(Length(P))]);
    exit;
  end;

  { 5. 只要本地固定硬盘：U 盘、光驱、网络盘、内存盘都不行。 }
  DriveType := GetDriveTypeW(Copy(P, 1, 3));
  if DriveType <> DRIVE_FIXED then
  begin
    Reason := FmtMessage(CustomMessage('ErrNotFixed'), [Copy(P, 1, 3)]);
    exit;
  end;

  { 6. 系统目录。常量和环境变量都看，任一处命中就拦。 }
  SetArrayLength(SystemDirs, 9);
  SystemDirs[0] := ExpandConstant('{commonpf64}');
  SystemDirs[1] := ExpandConstant('{commonpf32}');
  SystemDirs[2] := ExpandConstant('{win}');
  SystemDirs[3] := ExpandConstant('{commonappdata}');
  SystemDirs[4] := GetEnv('ProgramFiles');
  SystemDirs[5] := GetEnv('ProgramFiles(x86)');
  SystemDirs[6] := GetEnv('ProgramW6432');
  SystemDirs[7] := GetEnv('ProgramData');
  SystemDirs[8] := GetEnv('SystemRoot');
  if UnderAny(P, SystemDirs, Hit) then
  begin
    Reason := FmtMessage(CustomMessage('ErrSystemDir'), [Hit]);
    exit;
  end;

  { 7. OneDrive 同步目录。 }
  SetArrayLength(OneDriveDirs, 3);
  OneDriveDirs[0] := GetEnv('OneDrive');
  OneDriveDirs[1] := GetEnv('OneDriveConsumer');
  OneDriveDirs[2] := GetEnv('OneDriveCommercial');
  if UnderAny(P, OneDriveDirs, Hit) then
  begin
    Reason := FmtMessage(CustomMessage('ErrOneDrive'), [Hit]);
    exit;
  end;

  { 8. 已经有一份 ModAVD：不覆盖。只剩数据（以前卸载时留下的）也不覆盖，提示换个目录。 }
  if FileExists(P + '\modavd-manifest.json') or
     (FileExists(P + '\mas-avd.json') and FileExists(P + '\ModAVD.exe')) then
  begin
    Reason := CustomMessage('ErrExisting');
    exit;
  end;
  if FileExists(P + '\mas-avd.json') then
  begin
    Reason := CustomMessage('ErrLeftover');
    exit;
  end;

  { 9. 已有的文件夹里放着别的东西：不装进去。否则卸载时选「连数据一起删」会连带删掉这些东西。
     不存在的文件夹、空的已有文件夹照常放行。 }
  if not DirIsEmptyOrMissing(P) then
  begin
    Reason := CustomMessage('ErrNotEmpty');
    exit;
  end;

  Result := True;
end;

function CheckDirAndLog(Dir: String; var Reason: String): Boolean;
begin
  Result := ValidateInstallDir(Dir, Reason);
  if Result then
    Log('Install folder accepted: ' + Dir)
  else
    Log('Install folder rejected: ' + Dir + ' -- ' + Reason);
end;

{ ---- 选目录页 ---------------------------------------------------------------------------------- }

procedure UpdateFreeSpace;
var
  Drive: String;
  Free, Total, Tenths: Int64;
begin
  if FreeSpaceLabel = nil then
    exit;
  Drive := ExtractFileDrive(WizardDirValue);
  if (Drive <> '') and GetSpaceOnDisk64(AddBackslash(Drive), Free, Total) then
  begin
    Tenths := Free div 107374182;
    { 续行不能以 [ 开头：Inno 会把它当成新的一节。 }
    FreeSpaceLabel.Caption := FmtMessage(CustomMessage('FreeSpace'), [AddBackslash(Drive),
      IntToStr(Tenths div 10) + '.' + IntToStr(Tenths mod 10)]);
  end
  else
    FreeSpaceLabel.Caption := CustomMessage('FreeSpaceUnknown');
end;

procedure DirEditChange(Sender: TObject);
begin
  if OldDirEditChange <> nil then
    OldDirEditChange(Sender);
  UpdateFreeSpace;
end;

procedure InitializeWizard;
var
  Note: TNewStaticText;
begin
  Note := TNewStaticText.Create(WizardForm);
  Note.Parent := WizardForm.SelectDirPage;
  Note.Left := WizardForm.DirEdit.Left;
  Note.Top := WizardForm.DirEdit.Top + WizardForm.DirEdit.Height + ScaleY(16);
  Note.Width := WizardForm.SelectDirPage.ClientWidth - Note.Left;
  Note.AutoSize := False;
  Note.WordWrap := True;
  Note.Height := ScaleY(44);
  Note.Font.Style := [fsBold];
  Note.Caption := CustomMessage('SelectDirNote');

  FreeSpaceLabel := TNewStaticText.Create(WizardForm);
  FreeSpaceLabel.Parent := WizardForm.SelectDirPage;
  FreeSpaceLabel.Left := Note.Left;
  FreeSpaceLabel.Top := Note.Top + Note.Height + ScaleY(8);
  FreeSpaceLabel.Width := Note.Width;
  FreeSpaceLabel.AutoSize := False;
  FreeSpaceLabel.Height := ScaleY(20);

  OldDirEditChange := WizardForm.DirEdit.OnChange;
  WizardForm.DirEdit.OnChange := @DirEditChange;
  UpdateFreeSpace;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Reason: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then
    if not CheckDirAndLog(WizardDirValue, Reason) then
    begin
      SuppressibleMsgBox(Reason, mbError, MB_OK, IDOK);
      Result := False;
    end;
end;

{ 静默安装不经过选目录页的「下一步」时，这里兜底：不合格就让安装失败，原因进日志（写文件之前）。 }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Reason: String;
begin
  Result := '';
  if not CheckDirAndLog(WizardDirValue, Reason) then
    Result := Reason;
end;

{ ---- 卸载 -------------------------------------------------------------------------------------- }

{ 根目录顶层可能出现的 ModAVD 自己的名字：程序、用户数据、管理器运行时写的、卸载程序自己。 }
function IsKnownTopName(Name: String): Boolean;
var
  N: String;
begin
  N := Lowercase(Name);
  Result := (N = 'avd') or (N = 'mas-avd.json') or (N = 'mas-avd.json.lock') or (N = 'logs') or
    (N = 'shader-cache') or (N = 'guest-patches') or (N = 'downloads') or (N = '.update') or (N = 'sdk') or
    (N = 'modavd.exe') or (N = 'modavd-manifest.json') or (N = 'tools') or (N = 'components') or
    (N = 'extras') or (N = 'mumu-shim') or (N = 'licenses') or
    ((Copy(N, 1, 5) = 'unins') and ((ExtractFileExt(N) = '.exe') or (ExtractFileExt(N) = '.dat')));
end;

{ 「连数据一起删」之前的保险：App 不是盘根、不在系统目录下；里面已经空了，或者还留着 mas-avd.json 或 avd，
  而且顶层每一项都是 ModAVD 自己的名字。有一项认不出来就整个不删（Why 里写是哪一项），宁可留下。 }
function DataDirSafeToDelete(App: String; var Why: String): Boolean;
var
  FindRec: TFindRec;
  HasMarker, IsEmpty: Boolean;
begin
  Result := False;
  Why := '';
  App := NormalizeDir(App);
  if not IsDriveLetterPath(App) or (Length(App) <= 3) or IsUnder(App, ExpandConstant('{win}')) or
     IsUnder(App, ExpandConstant('{commonpf64}')) or IsUnder(App, ExpandConstant('{commonpf32}')) or
     IsUnder(App, ExpandConstant('{commonappdata}')) then
  begin
    Why := 'not a safe folder to delete';
    exit;
  end;
  if not DirExists(App) then
  begin
    Why := 'folder is gone';
    exit;
  end;
  IsEmpty := True;
  if FindFirst(AddBackslash(App) + '*', FindRec) then
  try
    repeat
      if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
      begin
        IsEmpty := False;
        if not IsKnownTopName(FindRec.Name) then
        begin
          Why := 'unknown item ' + FindRec.Name;
          exit;
        end;
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
  HasMarker := FileExists(App + '\mas-avd.json') or DirExists(App + '\avd');
  if not (IsEmpty or HasMarker) then
  begin
    Why := 'no mas-avd.json or avd';
    exit;
  end;
  Result := True;
end;

{ 只在交互卸载时问；默认「否」。静默卸载一律不删数据。 }
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  App, Why: String;
begin
  App := NormalizeDir(ExpandConstant('{app}'));
  if CurUninstallStep = usUninstall then
  begin
    DeleteUserData := False;
    if not UninstallSilent then
      DeleteUserData := MsgBox(FmtMessage(CustomMessage('UninstDeleteData'), [App]),
        mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
    if DeleteUserData then
      Log('User chose to delete user data in ' + App)
    else
      Log('Keeping user data in ' + App);
  end
  else if (CurUninstallStep = usPostUninstall) and DeleteUserData then
  begin
    if DataDirSafeToDelete(App, Why) then
    begin
      Log('Deleting user data: ' + App);
      if not DelTree(App, True, True, True) then
        Log('Could not delete everything in ' + App);
    end
    else
      Log('Not deleting user data in ' + App + ': ' + Why);
  end;
end;

{ ---- 自检（/DSelfTest 编出来的才有） ------------------------------------------------------------ }

#ifdef SelfTest
var
  SelfTestFailures: Integer;

procedure Expect(Name: String; Ok: Boolean);
begin
  if Ok then
    Log('SELFTEST PASS ' + Name)
  else
  begin
    Log('SELFTEST FAIL ' + Name);
    SelfTestFailures := SelfTestFailures + 1;
  end;
end;

procedure ExpectDir(Name, Dir: String; WantOk: Boolean);
var
  Reason: String;
  Ok: Boolean;
begin
  Ok := ValidateInstallDir(Dir, Reason);
  Log('  ' + Dir + ' -> ' + Reason);
  Expect(Name, Ok = WantOk);
end;

procedure ExpectDelete(Name, Dir: String; WantOk: Boolean);
var
  Why: String;
  Ok: Boolean;
begin
  Ok := DataDirSafeToDelete(Dir, Why);
  Log('  ' + Dir + ' -> ' + Why);
  Expect(Name, Ok = WantOk);
end;

function InitializeSetup: Boolean;
var
  D, W: String;
begin
  SelfTestFailures := 0;
  { 整个校验用安装程序所在的盘（要是本地固定硬盘）。 }
  D := AddBackslash(ExtractFileDrive(ExpandConstant('{src}')));
  { 默认目录：各种盘符类型组合（0 未知、1 不存在、2 可移动、3 固定、4 网络、5 光驱、6 内存盘）。 }
  Expect('default D fixed', PickDefaultDir(3, 3) = 'D:\ModAVD');
  Expect('default D fixed, no E', PickDefaultDir(3, 1) = 'D:\ModAVD');
  Expect('default no D, E fixed', PickDefaultDir(1, 3) = 'E:\ModAVD');
  Expect('default D removable, E fixed', PickDefaultDir(2, 3) = 'E:\ModAVD');
  Expect('default D cdrom, E network', PickDefaultDir(5, 4) = 'C:\ModAVD');
  Expect('default D network, no E', PickDefaultDir(4, 1) = 'C:\ModAVD');
  Expect('default D ramdisk, E removable', PickDefaultDir(6, 2) = 'C:\ModAVD');
  Expect('default none', PickDefaultDir(1, 1) = 'C:\ModAVD');
  { 路径判断的纯函数部分。 }
  Expect('IsUnder same', IsUnder('C:\Program Files', 'c:\program files\'));
  Expect('IsUnder child', IsUnder('C:\Program Files\ModAVD', 'C:\Program Files'));
  Expect('IsUnder sibling prefix', not IsUnder('C:\Program Files Extra\ModAVD', 'C:\Program Files'));
  Expect('IsUnder empty base', not IsUnder('C:\x', ''));
  Expect('drive letter', IsDriveLetterPath('E:\x') and IsDriveLetterPath('E:') and not IsDriveLetterPath('\\srv\share\x'));
  Expect('non-ascii', HasNonAscii('E:\' + #$6A21 + #$62DF) and not HasNonAscii('E:\Mod AVD_1-2'));
  ExpectDir('ok', D + 'ModAVD-selftest', True);
  ExpectDir('root', D, False);
  { 不存在的盘：真装时 Inno 自己先拦（「驱动器不存在」），这里直接核我们的盘类型判断。 }
  ExpectDir('not fixed', 'Q:\ModAVD', False);
  ExpectDir('unc', '\\localhost\c$\ModAVD', False);
  ExpectDir('non-ascii', D + #$6A21#$62DF, False);
  ExpectDir('too long', D + StringOfChar('a', 78), False);
  ExpectDir('max len ok', D + StringOfChar('a', 77), True);
  ExpectDir('program files', ExpandConstant('{commonpf64}') + '\ModAVD', False);
  { 已有文件夹：空的、不存在的放行，放着别的东西的拦。卸载时「连数据一起删」的保险。
    临时目录建在自检程序旁边，核完删掉。 }
  W := ExpandConstant('{src}') + '\selftest-work';
  ForceDirectories(W + '\empty');
  ForceDirectories(W + '\full');
  SaveStringToFile(W + '\full\notes.txt', 'x', False);
  ExpectDir('empty existing dir', W + '\empty', True);
  ExpectDir('missing dir', W + '\missing', True);
  ExpectDir('non-empty unrelated dir', W + '\full', False);
  ForceDirectories(W + '\del-marker\avd\mas_0.avd');
  ForceDirectories(W + '\del-marker\logs');
  SaveStringToFile(W + '\del-marker\mas-avd.json', '{}', False);
  ForceDirectories(W + '\del-empty');
  ForceDirectories(W + '\del-unknown\avd');
  SaveStringToFile(W + '\del-unknown\mas-avd.json', '{}', False);
  SaveStringToFile(W + '\del-unknown\my-game-saves.zip', 'x', False);
  ForceDirectories(W + '\del-nomarker\logs');
  ExpectDelete('delete: marker + known names', W + '\del-marker', True);
  ExpectDelete('delete: empty', W + '\del-empty', True);
  ExpectDelete('delete: unknown item', W + '\del-unknown', False);
  ExpectDelete('delete: no marker', W + '\del-nomarker', False);
  ExpectDelete('delete: drive root', D, False);
  ExpectDelete('delete: windows', ExpandConstant('{win}') + '\ModAVD', False);
  DelTree(W, True, True, True);
  Log('SELFTEST DONE failures=' + IntToStr(SelfTestFailures));
  Result := False;
end;
#endif
