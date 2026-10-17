@echo off
rem First-time setup: verifies your copy of the game and builds the native game in this folder.
rem Extra arguments are passed through, e.g.  Setup.bat -Rebuild   or   Setup.bat -Image D:\TTT2.iso
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup\setup.ps1" %*
set RESULT=%ERRORLEVEL%
if not "%TT2_NO_PAUSE%"=="1" pause
exit /b %RESULT%
