@echo off
rem Asks for your name and room code again, then starts the game.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0launcher\coop-launcher.ps1" -Reset
