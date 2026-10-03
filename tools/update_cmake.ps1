# update_cmake.ps1 — sync a game project's CMakeLists.txt with the scaffold
# template's managed blocks (implementation: tools/update_cmake.py).
# Usage: update_cmake.ps1 [--project DIR] [--check] [--dry-run]
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Py = $env:PYTHON
if (-not $Py) {
    foreach ($c in 'python3', 'python', 'py') {
        if (Get-Command $c -ErrorAction SilentlyContinue) { $Py = $c; break }
    }
}
if (-not $Py) { Write-Error 'update_cmake: python not found on PATH'; exit 1 }
& $Py (Join-Path $Root 'tools/update_cmake.py') @args
exit $LASTEXITCODE
