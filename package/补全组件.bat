@echo off
rem Copyright 2026 qiyinxi
rem SPDX-License-Identifier: AGPL-3.0-or-later
rem Licensed under the GNU Affero General Public License v3.0 or later; see LICENSES\AGPL-3.0.txt in this folder.
rem Downloads adb and the Android system image with tools\modavd.exe (components fetch). Without arguments it asks
rem once where to download from (the menu is tools\components-sources.txt) and keeps the answer in
rem downloads\source.txt; delete that file to be asked again. Arguments go to modavd as they are, e.g. --mirror google.
setlocal
set "EXE=%~dp0tools\modavd.exe"
if not exist "%EXE%" goto noexe
if not "%~1"=="" goto withargs
set "SRCFILE=%~dp0downloads\source.txt"
set "SRC="
if exist "%SRCFILE%" set /p SRC=<"%SRCFILE%"
call :check
if not defined MIRROR call :ask
if "%MIRROR%"=="auto" goto auto
"%EXE%" --root "%~dp0." components fetch --mirror %MIRROR%
set "RC=%ERRORLEVEL%"
goto fetched
:auto
"%EXE%" --root "%~dp0." components fetch
set "RC=%ERRORLEVEL%"
:fetched
rem 2 = the license was not accepted: nothing is kept then, not even the choice.
if "%RC%"=="2" goto done
if not exist "%~dp0downloads" mkdir "%~dp0downloads" 2>nul
>"%SRCFILE%" echo %MIRROR%
if "%RC%"=="0" goto done
if "%MIRROR%"=="auto" goto done
echo.
echo Download source: %MIRROR%. To choose another one, delete downloads\source.txt and run this file again.
goto done
:withargs
"%EXE%" --root "%~dp0." components fetch %*
set "RC=%ERRORLEVEL%"
goto done
:noexe
echo tools\modavd.exe is missing. Extract the whole ModAVD package into this folder again, then run this file.
set "RC=1"
goto done
:done
pause
exit /b %RC%

rem MIRROR = SRC when it is one of the four choices, else empty (a damaged file asks again).
:check
setlocal EnableDelayedExpansion
set "M="
for %%m in (auto googledownloads_cn tencent google) do if /i "!SRC!"=="%%m" set "M=%%m"
endlocal & set "MIRROR=%M%"
exit /b 0

rem Shows the menu (UTF-8, so under code page 65001 just for that) and asks until the answer is one of the choices.
:ask
set "OLDCP="
for /f "tokens=*" %%a in ('chcp ^<nul') do for %%b in (%%a) do set "OLDCP=%%~nb"
chcp 65001 >nul <nul
type "%~dp0tools\components-sources.txt"
rem chcp gets <nul: with input redirected from a file it would eat that input. Back to the old code page before reading.
if defined OLDCP chcp %OLDCP% >nul <nul
:askagain
set "ANS="
set /p "ANS=> "
setlocal EnableDelayedExpansion
set "M="
if "!ANS!"=="" set "M=auto"
if "!ANS!"=="1" set "M=auto"
if "!ANS!"=="2" set "M=googledownloads_cn"
if "!ANS!"=="3" set "M=tencent"
if "!ANS!"=="4" set "M=google"
endlocal & set "MIRROR=%M%"
if not defined MIRROR goto askagain
exit /b 0
