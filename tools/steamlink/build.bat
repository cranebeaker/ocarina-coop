@echo off
rem Builds OOTSteamLink.exe and the mock steam_api64.dll (64 bit, MSVC). Run from any folder.
setlocal
set "HERE=%~dp0"
set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul || exit /b 1
if not exist "%HERE%out" mkdir "%HERE%out"
pushd "%HERE%out"
cl /nologo /D_CRT_SECURE_NO_WARNINGS /O2 /W3 /Fe:OOTSteamLink.exe "%HERE%ootsteamlink.c" ws2_32.lib user32.lib || exit /b 1
cl /nologo /D_CRT_SECURE_NO_WARNINGS /O2 /W3 /LD /Fe:mock_steam_api64.dll "%HERE%mock\mock_steam_api.c" ws2_32.lib || exit /b 1
popd
echo Built %HERE%out
