@echo off
rem upload.cmd [Release|RelWithDebInfo] [-BuildDir build] [-SkipBuild] [-SkipSymbols]   builds, stages and uploads Lila to Steam app 5520720
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0upload.ps1" %*
