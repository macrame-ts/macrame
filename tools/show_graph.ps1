# Render a Graphviz .dot file to .svg (next to it) and open it with the default app.
# Usage:  .\tools\show_graph.ps1 [path\to\graph.dot]     (default: game_frame.dot)
# Generate the input with:  task_system --dot [path]
param([string]$DotFile = "game_frame.dot")

$dotExe = (Get-Command dot -ErrorAction SilentlyContinue).Source
if (-not $dotExe) { $dotExe = "C:\Program Files\Graphviz\bin\dot.exe" }
if (-not (Test-Path $dotExe))
{
    Write-Error "Graphviz 'dot' not found; install with: winget install Graphviz.Graphviz"
    exit 1
}
if (-not (Test-Path $DotFile))
{
    Write-Error "'$DotFile' not found; generate it with: task_system --dot"
    exit 1
}

$svg = [System.IO.Path]::ChangeExtension($DotFile, ".svg")
& $dotExe -Tsvg $DotFile -o $svg
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "wrote $svg"
Invoke-Item $svg
