@echo off
call "%~dp0\..\scripts\th08vars.bat"
%*
if %errorlevel% neq 0 exit /b %errorlevel%
