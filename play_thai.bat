@echo off
rem Uplink (recompiled port) in Thai. Saves go to run\save.
cd /d "%~dp0run"
"..\build\uplink_port.exe" ..\.. save --lang th
