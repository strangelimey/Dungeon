@echo off
rem Double-clickable shim for blender-bridge.ps1 — all the logic lives there.
rem (A previous version did the Blender discovery inline; cmd's caret escaping
rem inside for /f ate the regex anchors, so the work moved to PowerShell.)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0blender-bridge.ps1" %*
