@echo off
setlocal

rem Installs the built VST3 and CLAP into the machine-wide plugin folders.
rem
rem MUST BE RUN FROM AN ELEVATED PROMPT -- both targets live under Program
rem Files. Right-click your terminal and choose "Run as administrator".
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

net session >nul 2>&1
if errorlevel 1 (
  echo This needs an elevated prompt -- Program Files is not writable otherwise.
  echo Right-click your terminal and choose "Run as administrator", then re-run.
  exit /b 1
)

if not exist "%SRC%\Seedlathe.vst3" (
  echo No build found at "%SRC%". Build the VST3 and CLAP targets first.
  exit /b 1
)

echo Installing VST3 to "%VST3DIR%\Seedlathe.vst3"
xcopy /E /I /Y /Q "%SRC%\Seedlathe.vst3" "%VST3DIR%\Seedlathe.vst3" >nul
if errorlevel 1 goto failed

if exist "%SRC%\Seedlathe.clap" (
  echo Installing CLAP to "%CLAPDIR%\Seedlathe.clap"
  if not exist "%CLAPDIR%" mkdir "%CLAPDIR%"
  copy /Y "%SRC%\Seedlathe.clap" "%CLAPDIR%\Seedlathe.clap" >nul
  if errorlevel 1 goto failed
)

echo.
echo Done. Rescan plugins in your host.
exit /b 0

:failed
echo.
echo Copy failed. If the plugin is loaded in a running host, close it first --
echo the file is locked while a host has it open.
exit /b 1
