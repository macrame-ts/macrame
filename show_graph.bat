@echo off
setlocal
rem Render a Graphviz .dot file to .svg (next to it) and open it with the default app.
rem Usage:  show_graph.bat [path\to\graph.dot]
rem No argument: renders sample_game_frame.dot AND opens sample_game_frame_avg.svg (the
rem average-run trace) if present -- the pair task_system --dot / --trace produces.
rem If Graphviz is missing, offers to install it via winget.

set "DOTFILE=%~1"
if "%DOTFILE%"=="" set "DOTFILE=sample_game_frame.dot"

set "DOTEXE=dot"
where dot >nul 2>nul
if not errorlevel 1 goto have_dot
set "DOTEXE=C:\Program Files\Graphviz\bin\dot.exe"
if exist "%DOTEXE%" goto have_dot
call :install_graphviz
if errorlevel 1 exit /b 1
:have_dot

if not exist "%DOTFILE%" (
    echo '%DOTFILE%' not found; generate it with: task_system --dot
    exit /b 1
)

set "SVGFILE=%DOTFILE:.dot=.svg%"
"%DOTEXE%" -Tsvg "%DOTFILE%" -o "%SVGFILE%"
if errorlevel 1 exit /b 1
echo wrote %SVGFILE%
start "" "%SVGFILE%"
if not "%~1"=="" exit /b 0
if exist "sample_game_frame_avg.svg" start "" "sample_game_frame_avg.svg"
exit /b 0

:install_graphviz
where winget >nul 2>nul
if errorlevel 1 (
    echo Graphviz 'dot' not found, and winget is unavailable to install it.
    echo Install Graphviz manually: https://graphviz.org/download/
    exit /b 1
)
choice /m "Graphviz 'dot' not found. Install it now via winget"
if errorlevel 2 (
    echo Skipped. Install later with: winget install Graphviz.Graphviz
    exit /b 1
)
winget install --id Graphviz.Graphviz -e --accept-source-agreements --accept-package-agreements
if errorlevel 1 (
    echo winget install failed; install Graphviz manually: https://graphviz.org/download/
    exit /b 1
)
if not exist "C:\Program Files\Graphviz\bin\dot.exe" (
    echo Installed, but dot.exe is not at the expected path yet -- open a new terminal so PATH refreshes, then re-run.
    exit /b 1
)
exit /b 0
