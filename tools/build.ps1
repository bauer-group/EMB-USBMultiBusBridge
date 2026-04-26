# SPDX-License-Identifier: Apache-2.0
#
# Windows wrapper for tools/build.sh. Forwards every argument verbatim
# to bash, which on Windows comes from either Git for Windows or WSL2
# (whichever is on PATH first). Docker Desktop with WSL2 backend is
# the supported configuration.
#
# Examples:
#   .\tools\build.ps1                # firmware + driver + userspace
#   .\tools\build.ps1 firmware pico2
#   .\tools\build.ps1 shell

$ErrorActionPreference = 'Stop'

$bash = Get-Command bash -ErrorAction SilentlyContinue
if (-not $bash) {
    Write-Error "bash not found on PATH. Install Git for Windows or enable WSL2."
    exit 2
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$script   = Join-Path $repoRoot 'tools/build.sh'

# Convert Windows path to a form bash understands (Git Bash / WSL both
# accept forward-slash POSIX-style paths to local files).
$scriptPosix = $script -replace '\\','/'

& bash $scriptPosix @args
exit $LASTEXITCODE
