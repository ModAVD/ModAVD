; ModAVD Setup's own messages (English). Kept entry for entry with messages-zh.isl: change one, change the other.
; %1, %2 are arguments, %n is a line break.

[CustomMessages]
SelectDirNote=ModAVD is installed here, and your instances and game data are stored here too. Each instance takes from a few GB to tens of GB, so pick a drive with enough room.
FreeSpace=%1 has %2 GB free.
FreeSpaceUnknown=Could not read how much free space this drive has.
ErrNotLocal=Please choose a folder on a local drive, for example D:\ModAVD. Network paths do not work.
ErrNonAscii=The path must contain only English letters, digits and common symbols; the emulator cannot read paths with Chinese or other non-ASCII characters. Please choose another path, for example D:\ModAVD.
ErrDriveRoot=ModAVD cannot be installed directly in the root of %1. Please use a folder inside it, for example %1ModAVD.
ErrTooLong=The path is too long: at most %1 characters, this one has %2. Please choose a shorter path, for example D:\ModAVD.
ErrNotFixed=%1 is not a fixed local disk (USB drives, optical drives and network drives do not work). Please choose another drive.
ErrSystemDir=ModAVD cannot be installed inside %1: writing there needs administrator rights, and instances stored there run into problems. Please choose another location, for example D:\ModAVD.
ErrOneDrive=ModAVD cannot be installed in a folder synced by OneDrive (%1): instance disk files are very large, syncing them fills up space and bandwidth and can corrupt files in use. Please choose another location.
ErrExisting=ModAVD is already installed here. Please open ModAVD.exe in this folder and use Update in Settings; Setup will not overwrite it.
ErrLeftover=This folder holds data left by an earlier ModAVD (mas-avd.json), and Setup will not overwrite it. Please choose an empty folder, or move this folder away first.
UninstDeleteData=Also delete your instances and game data?%n%nYes: delete the whole folder %1, including all instances, game data and logs. This cannot be undone.%nNo: remove only the program and keep your instances and game data where they are.
ErrNotEmpty=This folder already has other things in it. Please choose an empty folder or a new one, for example D:\ModAVD.
ErrReparse=%1 in this path is a junction or symbolic link that points to another folder, so the files would actually end up somewhere else. Please choose a regular folder, for example D:\ModAVD.
ErrShortName=%1 in this path is an abbreviated (8.3 short) folder name; the full name is %2. Please type the full name, or choose another path, for example D:\ModAVD.
ErrBadName=The folder name "%1" in this path ends in a space or a period, which Windows silently drops, so the files would end up in a different folder. Please remove the trailing spaces and periods, for example D:\ModAVD.
ErrReservedName=The folder name "%1" in this path is a name Windows reserves for devices (CON, PRN, AUX, NUL, COM0 to COM9, LPT0 to LPT9, also with an extension), so this folder cannot be created. Please choose another name, for example D:\ModAVD.
ErrBadChars=The path cannot contain any of these characters: : * ? " < > | (a colon is only allowed right after the drive letter). Please choose another path, for example D:\ModAVD.
