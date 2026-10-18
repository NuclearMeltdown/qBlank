@echo off
rem Quick syntax check: compiles the given source files to objects only.
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

call "%ROOT%\findvs.bat"
if errorlevel 1 exit /b 1

if not exist "%ROOT%\build\obj" mkdir "%ROOT%\build\obj"
cl /nologo /c /EHsc /std:c++17 /W3 /permissive- /Zc:__cplusplus /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DIMGUI_DISABLE_OBSOLETE_FUNCTIONS /DIMGUI_DISABLE_DEFAULT_FONT_VECTOR /DIMGUI_DISABLE_DEBUG_TOOLS /I"%ROOT%\src" /I"%ROOT%\third_party\imgui" /I"%ROOT%\third_party\imgui\backends" /Fo"%ROOT%\build\obj\\" %*
exit /b %errorlevel%
