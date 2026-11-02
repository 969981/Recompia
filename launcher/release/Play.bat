@echo off
rem Starts Pokopia from this folder without the launcher. Every path (exefs, romfs, shader cache) comes from pkp.ini next to
rem pkp.exe, relative paths resolved against this folder: run the launcher's setup once (it writes pkp.ini and builds
rem shader_cache\spirv). Saves stay in .\save, as with the launcher.
cd /d "%~dp0"
if not defined NXRT_FS_SAVE_DIR set "NXRT_FS_SAVE_DIR=%~dp0save"
start "" pkp.exe --log pkp.log
