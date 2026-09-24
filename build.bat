@echo off
rem Baut qBlank.
rem
rem   build.bat            Release, raeumt danach auf: es bleibt nur qBlank.exe
rem   build.bat keep       Release, behaelt den build-Ordner fuer schnelle Neubauten
rem   build.bat debug      Debug-Build, behaelt den build-Ordner
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "CFG=Release"
set "CLEAN=1"
if /i "%~1"=="debug" set "CFG=Debug"
if /i "%~1"=="debug" set "CLEAN=0"
if /i "%~1"=="keep" set "CLEAN=0"

call "%ROOT%\findvs.bat"
if errorlevel 1 exit /b 1

set "NINJA=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

rem Das Vulkan SDK fuer den Vulkan-Renderer: aus der Umgebung, sonst ein SDK,
rem das nur hineinkopiert wurde (Installer mit copy_only=1, ohne Registry und
rem Umgebungsvariable) und neben dem Repo in VulkanSDK\<Version> liegt -- die
rem hoechste Version gewinnt. Ohne SDK baut CMake mit Direct3D 11 allein.
if defined VULKAN_SDK goto :vulkanset
for /d %%d in ("%ROOT%\..\VulkanSDK\*") do if exist "%%~fd\Include\vulkan\vulkan.h" set "VULKAN_SDK=%%~fd"
:vulkanset
if defined VULKAN_SDK echo === Vulkan SDK: %VULKAN_SDK% ===

echo === Konfiguriere (%CFG%) ===
if not exist "%NINJA%" goto :nonijna
"%VSCMAKE%" -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=%CFG%
goto :configured
:nonijna
"%VSCMAKE%" -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DCMAKE_BUILD_TYPE=%CFG%
:configured
if errorlevel 1 exit /b 1

echo === Baue ===
"%VSCMAKE%" --build "%ROOT%\build" --parallel
if errorlevel 1 exit /b 1

if not exist "%ROOT%\build\bin\qBlank.exe" goto :missing

copy /y "%ROOT%\build\bin\qBlank.exe" "%ROOT%\qBlank.exe" >nul
if errorlevel 1 exit /b 1

rem Die Medienquelle wird nicht mehr danebengelegt: sie steckt als Ressource in
rem der exe und wird beim Installieren der Kamera von dort herausgeschrieben.
rem Ein Release ist damit wieder eine einzige Datei -- plus den Migrator.
rem
rem Der liegt im Build in einem eigenen Ordner, weil er CapView.exe heisst: der
rem Updater in CapView 3.7 laedt das Asset mit genau diesem Namen. Ein Release
rem besteht aus beiden Dateien, und der Migrator wandert unveraendert von
rem Release zu Release mit.
if not exist "%ROOT%\build\bin\migrator\CapView.exe" goto :nomigrator
copy /y "%ROOT%\build\bin\migrator\CapView.exe" "%ROOT%\CapView.exe" >nul
if errorlevel 1 exit /b 1
:nomigrator

if "%CLEAN%"=="0" goto :kept

rem Alles retten, was zufaellig neben der exe liegt, bevor der Ordner faellt:
rem der ffmpeg-Download landet dort, und die Einstellungen tun es auch.
if not exist "%ROOT%\build\bin\ffmpeg" goto :noffmpeg
if exist "%ROOT%\ffmpeg" goto :noffmpeg
echo === Verschiebe ffmpeg neben die exe ===
move "%ROOT%\build\bin\ffmpeg" "%ROOT%\ffmpeg" >nul
:noffmpeg
if not exist "%ROOT%\build\bin\qBlank.json" goto :nojson
if exist "%ROOT%\qBlank.json" goto :nojson
move "%ROOT%\build\bin\qBlank.json" "%ROOT%\qBlank.json" >nul
:nojson

echo === Raeume auf ===
rmdir /s /q "%ROOT%\build"

echo.
echo === Fertig: %ROOT%\qBlank.exe ===
echo Daneben CapView.exe: der Migrator, gehoert mit ins Release.
echo Einstellungen landen in qBlank.json daneben. Sonst wird nichts angelegt.
exit /b 0

:kept
echo.
echo === Fertig: %ROOT%\qBlank.exe   build-Ordner behalten ===
exit /b 0

:missing
echo FEHLER: qBlank.exe wurde nicht erzeugt.
exit /b 1
