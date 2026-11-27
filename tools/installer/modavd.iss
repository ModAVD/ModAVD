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
;   /DPayloadTopNames=<a|b|...>   载荷顶层的名字，用 | 隔开（文件名里不会有 |）。卸载时「连数据一起删」认哪些是程序，
;                                 就看这份；build-installer.ps1 从载荷目录读出来传进来
; 可选：
;   /DSelfTest                    编出只跑自检的安装程序：校验和默认目录的逻辑在日志里逐条核对，然后退出，不装任何东西。
;                                 不带载荷，PayloadTopNames 用下面那份固定的假清单
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
  #ifndef PayloadTopNames
    #error 缺少 /DPayloadTopNames
  #endif
#else
  #ifndef PayloadTopNames
    ; 自检用的假清单：只用来核 IsKnownTopName 的拼接和查找，不代表真包里有什么。
    #define PayloadTopNames "ModAVD.exe|modavd-manifest.json|sdk|tools"
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
; 发版包顶层新加程序目录时要在这里加一行，否则以后自更新放进去的文件卸载时会留下。
; （卸载时「连数据一起删」认程序名字用的是 PayloadTopNames，不用改这里。）
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
  INVALID_FILE_ATTRIBUTES = $FFFFFFFF;
  { 载荷顶层的名字（编译时由 build-installer.ps1 传进来），用 | 隔开。 }
  PayloadTopNames = '{#PayloadTopNames}';

var
  FreeSpaceLabel: TNewStaticText;
  OldDirEditChange: TNotifyEvent;
  DeleteUserData: Boolean;

function GetDriveTypeW(lpRootPathName: String): Cardinal;
  external 'GetDriveTypeW@kernel32.dll stdcall';
{ 对重解析点返回的是链接本身的属性，不跟过去。 }
function GetFileAttributesW(lpFileName: String): Cardinal;
  external 'GetFileAttributesW@kernel32.dll stdcall';
function GetLongPathNameW(lpszShortPath: String; lpszLongPath: String; cchBuffer: Cardinal): Cardinal;
  external 'GetLongPathNameW@kernel32.dll stdcall';

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

{ P（盘符路径，不是盘根）从盘根往下逐级看已经存在的每一级：有一级是重解析点（目录联接、符号链接、挂载的盘）
  就返回 True，Hit 是那一级。Deepest 是最深的、已经存在的那一级（一级都不存在时为空）。
  上面这些字符串比较（IsUnder 等）只看写出来的路径，联接会把它指到别处，所以要在这里拦。只读。 }
function FindReparseOnPath(P: String; var Hit, Deepest: String): Boolean;
var
  Rest, Cur, Part: String;
  I: Integer;
  Attr: Cardinal;
begin
  Result := False;
  Hit := '';
  Deepest := '';
  P := NormalizeDir(P);
  Cur := Copy(P, 1, 2);
  Rest := Copy(P, 4, Length(P));
  while Rest <> '' do
  begin
    I := Pos('\', Rest);
    if I = 0 then
    begin
      Part := Rest;
      Rest := '';
    end
    else
    begin
      Part := Copy(Rest, 1, I - 1);
      Rest := Copy(Rest, I + 1, Length(Rest));
    end;
    Cur := Cur + '\' + Part;
    Attr := GetFileAttributesW(Cur);
    { 这一级不存在（或者读不到），再往下的也不会存在。 }
    if Attr = INVALID_FILE_ATTRIBUTES then
      exit;
    Deepest := Cur;
    if (Attr and FILE_ATTRIBUTE_REPARSE_POINT) <> 0 then
    begin
      Hit := Cur;
      Result := True;
      exit;
    end;
  end;
end;

{ 已经存在的路径的完整长名（把 PROGRA~1 这种 8.3 短名换回 Program Files）。取不到时返回空。 }
function GetLongPath(P: String): String;
var
  Len: Cardinal;
begin
  Result := StringOfChar(' ', 1024);
  Len := GetLongPathNameW(P, Result, 1024);
  if (Len = 0) or (Len >= 1024) then
    Result := ''
  else
    SetLength(Result, Len);
end;

{ 交互时在选目录页点「下一步」调，静默安装时在 PrepareToInstall 再调一次。合格返回 True；不合格返回 False，
  Reason 是给用户看的一句话。只读，不建目录也不写文件。 }
function ValidateInstallDir(Dir: String; var Reason: String): Boolean;
var
  P, Hit, Deepest, LongName: String;
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

  { 5a. 路径上已经存在的某一级是联接或符号链接：实际会装到它指向的地方，下面按字符串比的检查都看不出来。 }
  if FindReparseOnPath(P, Hit, Deepest) then
  begin
    Reason := FmtMessage(CustomMessage('ErrReparse'), [Hit]);
    exit;
  end;

  { 5b. 8.3 短名（C:\PROGRA~1 这种）：已经存在的那几级，长名和写的不一样（不分大小写）就拦，
     否则下面的系统目录检查会被绕过去。 }
  if Deepest <> '' then
  begin
    LongName := GetLongPath(Deepest);
    if LongName = '' then
      Log('Could not read the long name of ' + Deepest)
    else if Lowercase(LongName) <> Lowercase(Deepest) then
    begin
      Reason := FmtMessage(CustomMessage('ErrShortName'), [Deepest, LongName]);
      exit;
    end;
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

{ Inno 自己建的卸载程序：unins 加三位数字，再加 .exe 或 .dat（unins000.exe、unins000.dat）。N 已转小写。 }
function IsUninstallerName(N: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  if (Length(N) <> 12) or (Copy(N, 1, 5) <> 'unins') then
    exit;
  for I := 6 to 8 do
    if (N[I] < '0') or (N[I] > '9') then
      exit;
  Result := (Copy(N, 9, 4) = '.exe') or (Copy(N, 9, 4) = '.dat');
end;

{ 根目录顶层可能出现的 ModAVD 自己的名字：
  - 程序：这个安装程序的载荷顶层有的名字（PayloadTopNames，编译时从载荷读出来）；
  - 用户数据和管理器运行时写的：avd、mas-avd.json（和 .lock）、logs、shader-cache、guest-patches、downloads、.update；
  - 卸载程序自己。 }
function IsKnownTopName(Name: String): Boolean;
var
  N: String;
begin
  N := Lowercase(Name);
  Result := False;
  if N = '' then
    exit;
  Result := (N = 'avd') or (N = 'mas-avd.json') or (N = 'mas-avd.json.lock') or (N = 'logs') or
    (N = 'shader-cache') or (N = 'guest-patches') or (N = 'downloads') or (N = '.update') or
    IsUninstallerName(N) or (Pos('|' + N + '|', '|' + Lowercase(PayloadTopNames) + '|') > 0);
end;

{ 「连数据一起删」之前的保险：App 不是盘根、不在系统目录下；App 和它上面的每一级都不是重解析点；里面已经空了，
  或者还留着 mas-avd.json 或 avd，而且顶层每一项都是 ModAVD 自己的名字、都不是重解析点。有一项不对就整个不删
  （Why 里写是哪一项），宁可留下。
  更深处的联接、符号链接不用另外拦：Inno 帮助里 DelTree 一节写明，它会删掉是重解析点的目录本身，但不进去删里面的
  东西（6.7.3 的帮助；自检里有一条实测）。顶层的照样拦住，是因为顶层的名字本来就该是我们自己建的普通文件夹。 }
function DataDirSafeToDelete(App: String; var Why: String): Boolean;
var
  FindRec: TFindRec;
  HasMarker, IsEmpty: Boolean;
  Hit, Deepest: String;
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
  if FindReparseOnPath(App, Hit, Deepest) then
  begin
    Why := 'reparse point (junction or symbolic link) on the path: ' + Hit;
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
        if (FindRec.Attributes and FILE_ATTRIBUTE_REPARSE_POINT) <> 0 then
        begin
          Why := 'reparse point (junction or symbolic link) ' + FindRec.Name;
          exit;
        end;
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

{ 校验不通过，而且原因正是 Want（核对拦下的理由没走错分支）。 }
procedure ExpectDirReason(Name, Dir, Want: String);
var
  Reason: String;
  Ok: Boolean;
begin
  Ok := ValidateInstallDir(Dir, Reason);
  Log('  ' + Dir + ' -> ' + Reason);
  Expect(Name, (not Ok) and (Reason = Want));
end;

function GetShortPathNameW(lpszLongPath: String; lpszShortPath: String; cchBuffer: Cardinal): Cardinal;
  external 'GetShortPathNameW@kernel32.dll stdcall';

function GetShortPath(P: String): String;
var
  Len: Cardinal;
begin
  Result := StringOfChar(' ', 1024);
  Len := GetShortPathNameW(P, Result, 1024);
  if (Len = 0) or (Len >= 1024) then
    Result := ''
  else
    SetLength(Result, Len);
end;

{ 建目录联接 Link -> Target（cmd 的 mklink /J，不要管理员权限）。建成返回 True。 }
function MakeJunction(Link, Target: String): Boolean;
var
  RC: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'), '/C mklink /J "' + Link + '" "' + Target + '"', '', SW_HIDE,
    ewWaitUntilTerminated, RC) and (RC = 0) and
    ((GetFileAttributesW(Link) and FILE_ATTRIBUTE_REPARSE_POINT) <> 0) and
    (GetFileAttributesW(Link) <> INVALID_FILE_ATTRIBUTES);
  if not Result then
    Log(Format('  mklink /J %s %s failed (exit code %d)', [Link, Target, RC]));
end;

{ 要在磁盘上建目录的那些用例。W 是一个刚建的空目录。 }
procedure RunDiskCases(W: String);
var
  S, LongDir: String;
begin
  { 已有文件夹：空的、不存在的放行，放着别的东西的拦。 }
  ForceDirectories(W + '\empty');
  ForceDirectories(W + '\full');
  SaveStringToFile(W + '\full\notes.txt', 'x', False);
  ExpectDir('empty existing dir', W + '\empty', True);
  ExpectDir('missing dir', W + '\missing', True);
  ExpectDir('missing nested dir', W + '\missing\a\b', True);
  ExpectDir('non-empty unrelated dir', W + '\full', False);

  { 8.3 短名：盘上关了短名时 GetShortPathName 原样返回，这时跳过。 }
  LongDir := W + '\LongFolderName123';
  ForceDirectories(LongDir);
  ExpectDir('long name, different case ok', Uppercase(W) + '\longfoldername123\ModAVD', True);
  S := GetShortPath(LongDir);
  if (S = '') or (Lowercase(S) = Lowercase(LongDir)) then
    Log('SELFTEST SKIP 8.3 alias: no 8.3 short names on this volume (' + LongDir + ' -> ' + S + ')')
  else
    ExpectDirReason('8.3 alias', S + '\ModAVD', FmtMessage(CustomMessage('ErrShortName'), [S, LongDir]));
  S := GetShortPath(ExpandConstant('{commonpf64}'));
  if (S = '') or (Lowercase(S) = Lowercase(ExpandConstant('{commonpf64}'))) then
    Log('SELFTEST SKIP 8.3 program files: no 8.3 short name for ' + ExpandConstant('{commonpf64}'))
  else
    ExpectDirReason('8.3 program files', S + '\ModAVD',
      FmtMessage(CustomMessage('ErrShortName'), [S, ExpandConstant('{commonpf64}')]));

  { 「连数据一起删」的保险。 }
  ForceDirectories(W + '\del-marker\avd\mas_0.avd');
  ForceDirectories(W + '\del-marker\logs');
  SaveStringToFile(W + '\del-marker\mas-avd.json', '{}', False);
  ForceDirectories(W + '\del-empty');
  ForceDirectories(W + '\del-unknown\avd');
  SaveStringToFile(W + '\del-unknown\mas-avd.json', '{}', False);
  SaveStringToFile(W + '\del-unknown\my-game-saves.zip', 'x', False);
  ForceDirectories(W + '\del-nomarker\logs');
  ForceDirectories(W + '\del-unins-ok\avd');
  SaveStringToFile(W + '\del-unins-ok\mas-avd.json', '{}', False);
  SaveStringToFile(W + '\del-unins-ok\unins000.exe', 'x', False);
  SaveStringToFile(W + '\del-unins-ok\unins000.dat', 'x', False);
  ForceDirectories(W + '\del-unins-notes\avd');
  SaveStringToFile(W + '\del-unins-notes\mas-avd.json', '{}', False);
  SaveStringToFile(W + '\del-unins-notes\unins000.exe', 'x', False);
  SaveStringToFile(W + '\del-unins-notes\uninstall-notes.txt', 'x', False);
  ExpectDelete('delete: marker + known names', W + '\del-marker', True);
  ExpectDelete('delete: empty', W + '\del-empty', True);
  ExpectDelete('delete: unknown item', W + '\del-unknown', False);
  ExpectDelete('delete: no marker', W + '\del-nomarker', False);
  ExpectDelete('delete: unins000.exe/.dat ok', W + '\del-unins-ok', True);
  ExpectDelete('delete: uninstall-notes.txt blocks', W + '\del-unins-notes', False);

  { 目录联接：建不出来就跳过（日志里写原因）。 }
  ForceDirectories(W + '\jtarget');
  if not MakeJunction(W + '\jn', W + '\jtarget') then
    Log('SELFTEST SKIP junction cases: could not create a junction with mklink /J')
  else
  begin
    ExpectDirReason('junction in path', W + '\jn\ModAVD', FmtMessage(CustomMessage('ErrReparse'), [W + '\jn']));
    ExpectDirReason('junction is the folder', W + '\jn', FmtMessage(CustomMessage('ErrReparse'), [W + '\jn']));
    ExpectDelete('delete: app folder is a junction', W + '\jn', False);
    ExpectDelete('delete: junction above app folder', W + '\jn\sub', False);

    ForceDirectories(W + '\del-reparse\avd');
    SaveStringToFile(W + '\del-reparse\mas-avd.json', '{}', False);
    if MakeJunction(W + '\del-reparse\logs', W + '\jtarget') then
      ExpectDelete('delete: top-level junction blocks', W + '\del-reparse', False)
    else
      Log('SELFTEST SKIP top-level junction case: could not create the junction');

    { Inno 帮助说 DelTree 删掉是重解析点的目录本身、不进去删里面的东西；这里实测一次。 }
    ForceDirectories(W + '\keep');
    SaveStringToFile(W + '\keep\keep.txt', 'x', False);
    ForceDirectories(W + '\deltree-deep\avd');
    if MakeJunction(W + '\deltree-deep\avd\lnk', W + '\keep') then
    begin
      DelTree(W + '\deltree-deep', True, True, True);
      Expect('DelTree removes a nested junction but not its target',
        (not DirExists(W + '\deltree-deep')) and FileExists(W + '\keep\keep.txt'));
    end
    else
      Log('SELFTEST SKIP DelTree junction case: could not create the junction');
  end;
end;

function InitializeSetup: Boolean;
var
  D, W: String;
  I: Integer;
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
  { 顶层名字：卸载程序只认 unins + 三位数字 + .exe/.dat；载荷名字来自 PayloadTopNames（自检是假清单）。 }
  Expect('name unins000.exe', IsKnownTopName('unins000.exe'));
  Expect('name UNINS001.DAT', IsKnownTopName('UNINS001.DAT'));
  Expect('name uninstall-notes.txt', not IsKnownTopName('uninstall-notes.txt'));
  Expect('name unins.exe', not IsKnownTopName('unins.exe'));
  Expect('name unins0001.exe', not IsKnownTopName('unins0001.exe'));
  Expect('name unins00a.exe', not IsKnownTopName('unins00a.exe'));
  Expect('name unins000.msg', not IsKnownTopName('unins000.msg'));
  Expect('name payload Tools', IsKnownTopName('Tools'));
  Expect('name payload ModAVD.exe', IsKnownTopName('ModAVD.exe'));
  Expect('name payload prefix only', not IsKnownTopName('tool'));
  Expect('name user data', IsKnownTopName('shader-cache') and IsKnownTopName('mas-avd.json.lock') and IsKnownTopName('.update'));
  Expect('name unknown', not IsKnownTopName('my-game-saves.zip') and not IsKnownTopName(''));
  ExpectDir('ok', D + 'ModAVD-selftest', True);
  ExpectDir('root', D, False);
  { 不存在的盘：真装时 Inno 自己先拦（「驱动器不存在」），这里直接核我们的盘类型判断。 }
  ExpectDir('not fixed', 'Q:\ModAVD', False);
  ExpectDir('unc', '\\localhost\c$\ModAVD', False);
  ExpectDir('non-ascii', D + #$6A21#$62DF, False);
  ExpectDir('too long', D + StringOfChar('a', 78), False);
  ExpectDir('max len ok', D + StringOfChar('a', 77), True);
  ExpectDir('program files', ExpandConstant('{commonpf64}') + '\ModAVD', False);
  ExpectDelete('delete: drive root', D, False);
  ExpectDelete('delete: windows', ExpandConstant('{win}') + '\ModAVD', False);

  { 要建目录的用例：工作目录放在同一个盘的根下，路径短（放在自检程序旁边会超过 80 个字符），核完一定删掉。 }
  W := '';
  for I := 1 to 20 do
    if W = '' then
    begin
      W := D + 'mst-' + IntToStr(Random(1000000));
      if DirExists(W) or FileExists(W) then
        W := '';
    end;
  if (W = '') or not CreateDir(W) then
  begin
    Log('SELFTEST FAIL could not create a work folder under ' + D);
    SelfTestFailures := SelfTestFailures + 1;
  end
  else
  begin
    Log('Self-test work folder: ' + W);
    try
      RunDiskCases(W);
    finally
      DelTree(W, True, True, True);
      if DirExists(W) then
      begin
        Log('SELFTEST FAIL could not remove the work folder ' + W);
        SelfTestFailures := SelfTestFailures + 1;
      end;
    end;
  end;
  Log('SELFTEST DONE failures=' + IntToStr(SelfTestFailures));
  Result := False;
end;
#endif
