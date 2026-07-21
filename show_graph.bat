@echo off
setlocal
rem Render a Graphviz .dot file to .svg (next to it) and open it with the default app.
rem Usage:  show_graph.bat [path\to\graph.dot]     (default: game_frame.dot)
rem Generate the input with:  task_system --dot [path]

set "DOTFILE=%~1"
if "%DOTFILE%"=="" set "DOTFILE=game_frame.dot"

set "DOTEXE=dot"
where dot >nul 2>nul
if errorlevel 1 set "DOTEXE=C:\Program Files\Graphviz\bin\dot.exe"
if not "%DOTEXE%"=="dot" if not exist "%DOTEXE%" (
    echo Graphviz 'dot' not found; install with: winget install Graphviz.Graphviz
    exit /b 1
)

if not exist "%DOTFILE%" (
    echo '%DOTFILE%' not found; generate it with: task_system --dot
    exit /b 1
)

set "SVGFILE=%DOTFILE:.dot=.svg%"
"%DOTEXE%" -Tsvg "%DOTFILE%" -o "%SVGFILE%"
if errorlevel 1 exit /b 1
echo wrote %SVGFILE%
start "" "%SVGFILE%"
