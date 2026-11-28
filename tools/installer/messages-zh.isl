; ModAVD 安装程序自己的提示（简体中文）。和 messages-en.isl 逐条对应：改一份就改另一份。
; %1、%2 是参数，%n 是换行。

[CustomMessages]
SelectDirNote=程序装在这里，实例和游戏数据也放在这里。每个实例要占几 GB 到几十 GB，请选空间够的盘。
FreeSpace=%1 现在还剩 %2 GB。
FreeSpaceUnknown=读不到这个盘还剩多少空间。
ErrNotLocal=请选本机硬盘上的文件夹，比如 D:\ModAVD。网络路径不行。
ErrNonAscii=路径里不能有中文或其他非英文字符，模拟器读不了这样的路径。请换一个只用英文字母、数字的路径，比如 D:\ModAVD。
ErrDriveRoot=不能直接装在盘的根目录 %1，请在里面建一个文件夹，比如 %1ModAVD。
ErrTooLong=路径太长了：最多 %1 个字符，现在是 %2 个。请换一个短一点的路径，比如 D:\ModAVD。
ErrNotFixed=%1 不是本机的固定硬盘（U 盘、光驱、网络盘都不行），请换一个盘。
ErrSystemDir=不能装在 %1 里面：这里要管理员权限才能写，模拟器在这里存实例会出问题。请换一个位置，比如 D:\ModAVD。
ErrOneDrive=不能装在 OneDrive 同步的文件夹（%1）里：实例的磁盘文件很大，同步会占满空间和网络，还可能把正在用的文件弄坏。请换一个位置。
ErrExisting=这里已经装过 ModAVD。请打开里面的 ModAVD.exe，在「设置」里用「更新」；安装程序不会覆盖它。
ErrLeftover=这里有以前留下的 ModAVD 数据（mas-avd.json），安装程序不会覆盖它。请换一个空文件夹，或者先把这个文件夹挪走。
UninstDeleteData=要不要把实例和游戏数据也一起删掉？%n%n选「是」：删掉整个 %1，包括所有实例、游戏数据和日志，删了找不回来。%n选「否」：只删程序，实例和游戏数据留在原处。
ErrNotEmpty=这个文件夹里已经有别的东西了。请选一个空文件夹或新文件夹，比如 D:\ModAVD。
ErrReparse=路径里的 %1 是目录联接或符号链接，指向别的文件夹，装进去的文件实际会放到别处。请换一个普通的文件夹，比如 D:\ModAVD。
ErrShortName=路径里的 %1 是缩写的文件夹名（8.3 短名），完整的名字是 %2。请写完整的名字，或者换一个路径，比如 D:\ModAVD。
ErrBadName=路径里的文件夹名「%1」以空格或句点结尾，Windows 会悄悄去掉它们，实际装到的不是写的这个文件夹。请去掉结尾的空格和句点，比如 D:\ModAVD。
ErrReservedName=路径里的文件夹名「%1」是 Windows 留给设备用的名字（CON、PRN、AUX、NUL、COM0–COM9、LPT0–LPT9，带扩展名也算），建不出这个文件夹。请换一个名字，比如 D:\ModAVD。
ErrBadChars=路径里不能有这些字符：: * ? " < > |（冒号只能紧跟在盘符后面）。请换一个路径，比如 D:\ModAVD。
