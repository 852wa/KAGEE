@echo off
rem Commit all changes and push to GitHub (852wa/KAGEE). The work is done by tools\push.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\push.ps1" %*
