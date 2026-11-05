@echo off
rem Copyright 2026 qiyinxi
rem SPDX-License-Identifier: AGPL-3.0-or-later
rem Licensed under the GNU Affero General Public License v3.0 or later; see LICENSES\AGPL-3.0.txt in this folder.
rem Copies the computer and game info with tools\modavd.exe (report-config); arguments go to it as they are.
setlocal
set "EXE=%~dp0tools\modavd.exe"
if not exist "%EXE%" goto noexe
"%EXE%" --root "%~dp0." report-config %*
set "RC=%ERRORLEVEL%"
goto done
:noexe
echo tools\modavd.exe is missing. Extract the whole ModAVD package into this folder again, then run this file.
set "RC=1"
goto done
:done
pause
exit /b %RC%
