@echo off
setlocal

rem Installs the built VST3 and CLAP into the machine-wide plugin folders.
rem
rem Just run it. It asks for administrator access itself -- both targets live
rem under Program Files -- so there is nothing to set up first.
rem
rem Why this exists. iPlug2's post-build step copies to the per-user locations,
rem %LOCALAPPDATA%\Programs\Common\VST3 and ...\CLAP. Those are valid VST3
rem locations by the spec, but a host has to be told to look there, and being
rem told is not enough on its own: FL Studio records a plugin CLASS against
rem every search folder, and a folder added by hand through its Plugin Manager
rem is registered as VST2 (plugclass 0). It then scans that folder looking for
rem VST2 DLLs, sees a directory named Seedlathe.vst3, does not recognise it,
rem and skips it -- with no error and nothing in the scan log to explain it.
rem
rem The machine-wide folders are already registered with the right classes
rem (VST3 is plugclass 7, CLAP is 11), which is why every other plugin on a
rem typical system lives there. Installing alongside them avoids the whole
rem question.
rem
rem Re-run after every rebuild: this copies, it does not link. Use
rem   mklink /J "C:\Program Files\Common Files\VST3\Seedlathe.vst3" ^
rem             "<repo>\build\out\Seedlathe.vst3"
rem instead if you would rather each rebuild go live on its own.

set "REPO=%~dp0.."
set "SRC=%REPO%\build\out"
set "VST3DIR=%CommonProgramFiles%\VST3"
set "CLAPDIR=%CommonProgramFiles%\CLAP"
set "LOG=%TEMP%\seedlathe-install.log"

rem Elevate ourselves rather than telling the user to. Three installs in a row
rem were lost to a UAC prompt that opened behind another window and was never
rem answered, and the only evidence either way was a file that had not changed
rem -- hence the log, and the pause below on failure.
net session >nul 2>&1
if not errorlevel 1 goto elevated

echo Requesting administrator access...
echo elevation requested %DATE% %TIME%> "%LOG%"
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs" >nul 2>&1
if errorlevel 1 (
  echo.
  echo The administrator prompt was refused, so nothing was installed.
  echo Watch for it in the taskbar: it can open behind other windows.
  exit /b 1
)
echo Started elevated. See "%LOG%" for the result.
exit /b 0

:elevated
echo installing %DATE% %TIME%> "%LOG%"

if not exist "%SRC%\Seedlathe.vst3" (
  echo No build found at "%SRC%". Build the VST3 and CLAP targets first.
  echo FAILED: no build at "%SRC%">> "%LOG%"
  goto failed
)

echo Installing VST3 to "%VST3DIR%\Seedlathe.vst3"
xcopy /E /I /Y /Q "%SRC%\Seedlathe.vst3" "%VST3DIR%\Seedlathe.vst3" >>"%LOG%" 2>&1
if errorlevel 1 goto failed

if exist "%SRC%\Seedlathe.clap" (
  echo Installing CLAP to "%CLAPDIR%\Seedlathe.clap"
  if not exist "%CLAPDIR%" mkdir "%CLAPDIR%"
  copy /Y "%SRC%\Seedlathe.clap" "%CLAPDIR%\Seedlathe.clap" >>"%LOG%" 2>&1
  if errorlevel 1 goto failed
)

echo OK %DATE% %TIME%>> "%LOG%"
echo.
echo Done. Rescan plugins in your host.
exit /b 0

:failed
echo FAILED %DATE% %TIME%>> "%LOG%"
echo.
echo Copy failed. If the plugin is loaded in a running host, close it first --
echo the file is locked while a host has it open.
echo Details in "%LOG%".
echo.
pause
exit /b 1
