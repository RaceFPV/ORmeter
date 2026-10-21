#Requires -Version 5.1
<#
.SYNOPSIS
    Install the ORmeter host daemon for the current user.

.DESCRIPTION
    Materialises a runtime copy of the daemon under %LOCALAPPDATA%\ORmeter and
    makes it start at logon.

    Steps, in execution order:

      1. prepare a virtual environment and install the dependencies
      2. install the sources (stopping any running instance first)
      3. register the per-user autostart value (HKCU Run)
      4. launch the tray headless, via the BASE interpreter's pythonw.exe

    Step 1 runs BEFORE the running instance is stopped on purpose: the venv and
    its dependencies live beside the sources, not in them, so they can be
    prepared while the current daemon keeps running. A failed dependency install
    must never take a working daemon down.

    The runtime must live on a local Windows path. A WSL or UNC target is
    refused: at logon WSL may not be running, and pythonw.exe fails silently (no
    console, so nothing is printed and nothing appears to happen) -- the
    autostart would simply do nothing. Reading the *sources* from a UNC path is
    fine; that is a one-time copy.

    The script itself downloads nothing. Step 1 asks pip for the four declared
    dependencies, which does reach PyPI; pass -SkipDeps to skip that.

    Safe to re-run: sources are refreshed, the existing config is preserved, and
    any running instance is stopped before files are replaced.

.PARAMETER Source
    Directory holding the daemon sources. Defaults to the `daemon` directory
    beside this script.

.PARAMETER PythonExe
    Interpreter used to create the venv. Defaults to the first working of
    `py -3`, then `python` on PATH.

.PARAMETER NoStart
    Do not launch the tray when the install finishes.

.PARAMETER ApiKey
    OpenRouter API key to write to the daemon's .env. Optional: omit it and the
    installer will look for OPENROUTER_API_KEY in the environment, and otherwise
    tell you exactly where to put the key yourself. An existing .env is never
    overwritten.

.PARAMETER SkipDeps
    Skip the dependency install (offline re-install of the sources only).

.PARAMETER Uninstall
    Remove the autostart value and move the installed runtime aside. Nothing is
    deleted -- the runtime is renamed to app.uninstalled-<timestamp>.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File install-windows.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File install-windows.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string] $Source,
    [string] $PythonExe,
    [string] $ApiKey,
    [switch] $NoStart,
    [switch] $SkipDeps,
    [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'

# --- names and paths ------------------------------------------------------
# $RunValueName and the launch command format MUST match
# daemon/autostart_windows.py (_VALUE_NAME, _command), which is what the tray's
# "Start at login" toggle writes. Two sources of truth for one registry value is
# a bug waiting to happen, so step 3 delegates to that module rather than
# re-implementing the string.
$AppName      = 'ORmeter'
$InstallRoot  = Join-Path $env:LOCALAPPDATA $AppName
$AppDir       = Join-Path $InstallRoot 'app'
$VenvDir      = Join-Path $AppDir '.venv'
$ConfigPath   = Join-Path $InstallRoot 'config'
$EnvPath      = Join-Path $InstallRoot '.env'
$RunKeyPath   = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$RunValueName = 'ORmeter'
$TrayScript   = Join-Path $AppDir 'tray_windows.py'

$DaemonFiles = @(
    'meter_daemon.py',
    'tray_windows.py',
    'autostart_windows.py',
    'icon_assets.py',
    'requirements-windows.txt',
    'config.example'
)

function Write-Step { param([string] $Message) Write-Host "==> $Message" -ForegroundColor Cyan }
function Write-Ok   { param([string] $Message) Write-Host "    $Message" -ForegroundColor Green }
function Write-Warn { param([string] $Message) Write-Host "    $Message" -ForegroundColor Yellow }
function Fail       { param([string] $Message) Write-Host "ERROR: $Message" -ForegroundColor Red; exit 1 }

function Test-WslOrUncPath {
    <#
        True for a path a logon autostart cannot rely on: a UNC path
        (\\wsl.localhost\..., \\wsl$\...) or a WSL-internal /mnt/... path. Both
        are only valid while the distro is running.
    #>
    param([Parameter(Mandatory)][string] $Path)
    if ($Path -match '^\\\\' -or $Path -match '^//') { return $true }
    if ($Path -match '^/mnt/') { return $true }
    return $false
}

function Get-DaemonProcess {
    <#
        Running ORmeter tray processes.

        Two deliberate precautions:
          * filter on process NAME first -- matching only on the command line
            also matches any shell whose own command line quotes that path,
            including the shell running this script;
          * normalise separators on BOTH sides before comparing. The recorded
            command line can mix them (Join-Path emits backslashes, but the
            launcher may have been handed forward slashes), and an unnormalised
            -like silently matches nothing -- so the running instance is never
            stopped and the copy step then trips over locked files.
    #>
    $needle = $AppDir -replace '/', '\'
    Get-CimInstance Win32_Process -Filter "Name='pythonw.exe'" -ErrorAction SilentlyContinue |
        Where-Object {
            if (-not $_.CommandLine) { return $false }
            ($_.CommandLine -replace '/', '\') -like "*$needle*"
        }
}

function Stop-Daemon {
    $procs = @(Get-DaemonProcess)
    if ($procs.Count -eq 0) { return }
    foreach ($p in $procs) {
        Write-Ok "stopping running instance (PID $($p.ProcessId))"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 2
}

function Resolve-Python {
    <#
        Returns @{ Exe = <path or name>; Prefix = @(<args>) }.
        Prefers the Windows launcher, which targets a real install rather than
        whatever venv happens to be active, then `python` on PATH.
    #>
    param([string] $Requested)

    $candidates = @()
    if ($Requested) {
        $candidates += @{ Exe = $Requested; Prefix = @() }
    }
    else {
        if (Get-Command py.exe -ErrorAction SilentlyContinue) {
            $candidates += @{ Exe = 'py.exe'; Prefix = @('-3') }
        }
        if (Get-Command python.exe -ErrorAction SilentlyContinue) {
            $candidates += @{ Exe = 'python.exe'; Prefix = @() }
        }
    }

    foreach ($c in $candidates) {
        try {
            $ver = & $c.Exe @($c.Prefix + @('-c', 'import sys; print(sys.version_info[0])')) 2>$null
            if ("$ver".Trim() -eq '3') { return $c }
        }
        catch { }
    }
    return $null
}

# ---------------------------------------------------------------------------
# uninstall
# ---------------------------------------------------------------------------
if ($Uninstall) {
    Write-Step "Uninstalling $AppName"
    Stop-Daemon
    Remove-ItemProperty -Path $RunKeyPath -Name $RunValueName -ErrorAction SilentlyContinue
    Write-Ok "removed the '$RunValueName' autostart value"

    if (Test-Path $AppDir) {
        $aside = Join-Path $InstallRoot ("app.uninstalled-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
        Move-Item -Path $AppDir -Destination $aside
        Write-Ok "runtime moved to $aside (nothing deleted)"
    }
    Write-Host ""
    Write-Host "Config and log left in place under $InstallRoot"
    exit 0
}

# ---------------------------------------------------------------------------
# 0. resolve and guard
# ---------------------------------------------------------------------------
if (-not $Source) {
    $Source = Join-Path $PSScriptRoot 'daemon'
}
if (-not (Test-Path $Source)) {
    Fail "daemon sources not found at: $Source -- pass -Source <path-to-daemon-dir>."
}

Write-Step "Installing $AppName"
Write-Host "    source:  $Source"
Write-Host "    runtime: $AppDir"

if (Test-WslOrUncPath $AppDir) {
    Fail "refusing to install to a WSL/UNC path: $AppDir -- autostart runs at logon, when WSL may not be running, and pythonw.exe fails silently because it has no console. The runtime must be on a local Windows path."
}
if (Test-WslOrUncPath $Source) {
    Write-Warn "source is on a WSL/UNC path -- fine for a one-time copy, but it must"
    Write-Warn "not be the runtime location. Read-only access is all we need."
}

$py = Resolve-Python -Requested $PythonExe
if (-not $py) {
    Fail "no Python 3 interpreter found. Install one, or pass -PythonExe <path>."
}
$basePrefix  = (& $py.Exe @($py.Prefix + @('-c', 'import sys; print(sys.base_exec_prefix)'))).Trim()
$basePythonw = Join-Path $basePrefix 'pythonw.exe'
Write-Ok "python: $($py.Exe) (base prefix: $basePrefix)"
if (-not (Test-Path $basePythonw)) {
    Fail "expected the base interpreter's pythonw.exe at: $basePythonw"
}

# ---------------------------------------------------------------------------
# 1. virtual environment and dependencies
#    Deliberately before the stop: this does not touch the running daemon's
#    files, so a failure here leaves it running.
# ---------------------------------------------------------------------------
Write-Step "Step 1/4  virtual environment and dependencies"
# The venv lives INSIDE $AppDir because tray_windows.py bootstraps sys.path from
# "<its own dir>/.venv/Lib/site-packages" -- that is what lets the BASE
# interpreter's pythonw.exe resolve bleak/pystray/PIL at logon. Move it and the
# tray fails to import.
New-Item -ItemType Directory -Force -Path $AppDir | Out-Null
$venvPython = Join-Path $VenvDir 'Scripts\python.exe'
if (-not (Test-Path $venvPython)) {
    Write-Ok "creating $VenvDir"
    & $py.Exe @($py.Prefix + @('-m', 'venv', $VenvDir))
    if ($LASTEXITCODE -ne 0) { Fail "venv creation failed ($LASTEXITCODE)" }
}
else {
    Write-Ok "venv already present, reusing it"
}

if ($SkipDeps) {
    Write-Warn "-SkipDeps: not installing dependencies"
}
else {
    # Never assume pip is present: a venv made by `uv venv` has no pip at all,
    # and a bundled Python can omit it too. ensurepip ships with CPython, so it
    # bootstraps without fetching anything.
    # Probe pip by looking for it on disk, NOT by running it. A failing native
    # command writes to stderr, and with $ErrorActionPreference = 'Stop'
    # PowerShell promotes that to a TERMINATING error -- so a `& python -m pip`
    # probe does not return a status to test, it kills the installer outright.
    $pipDir = Join-Path $VenvDir 'Lib\site-packages\pip'
    if (-not (Test-Path $pipDir)) {
        Write-Ok "no pip in the venv -- bootstrapping it with ensurepip"
        & $venvPython -m ensurepip --upgrade *> $null
        if ($LASTEXITCODE -ne 0) { Fail "ensurepip failed ($LASTEXITCODE)" }
        if (-not (Test-Path $pipDir)) { Fail "pip still absent after ensurepip" }
    }

    Write-Ok "installing dependencies (this reaches PyPI)"
    & $venvPython -m pip install --disable-pip-version-check -q -r (Join-Path $Source 'requirements-windows.txt')
    if ($LASTEXITCODE -ne 0) { Fail "pip install failed ($LASTEXITCODE)" }

    & $venvPython -c "import bleak, httpx, pystray, PIL" 2>$null
    if ($LASTEXITCODE -ne 0) { Fail "dependency import check failed" }
    Write-Ok "bleak, httpx, pystray, Pillow all import"
}

# ---------------------------------------------------------------------------
# 2. install the sources
# ---------------------------------------------------------------------------
Write-Step "Step 2/4  install sources"
Stop-Daemon

foreach ($f in $DaemonFiles) {
    $from = Join-Path $Source $f
    if (-not (Test-Path $from)) { Fail "missing source file: $from" }
    Copy-Item -Path $from -Destination (Join-Path $AppDir $f) -Force
}
Write-Ok "copied $($DaemonFiles.Count) files"

# Config on first install only. Never clobber a live config -- that would
# silently reset wallet_reference.
if (-not (Test-Path $ConfigPath)) {
    Copy-Item -Path (Join-Path $Source 'config.example') -Destination $ConfigPath -Force
    Write-Ok "wrote default config to $ConfigPath"
}
else {
    Write-Ok "existing config preserved at $ConfigPath"
}

# API key. The daemon reads OPENROUTER_API_KEY from the environment, or from a
# .env beside its config -- so this is the one place a new install needs a
# credential, and it should not be left to a vague failure at first poll.
# Never clobber: a re-run must not destroy a key the user already has.
if (Test-Path $EnvPath) {
    Write-Ok "existing API key file preserved at $EnvPath"
}
elseif ($ApiKey) {
    Set-Content -Path $EnvPath -Value "OPENROUTER_API_KEY=$ApiKey" -Encoding ASCII -NoNewline
    Write-Ok "wrote the API key to $EnvPath"
}
elseif ($env:OPENROUTER_API_KEY) {
    Write-Ok "OPENROUTER_API_KEY is already in the environment -- no key file needed"
}
else {
    Write-Warn "no OpenRouter API key found, and the daemon needs one."
    Write-Warn "  Create a key at https://openrouter.ai/keys then either:"
    Write-Warn "    * set OPENROUTER_API_KEY in your environment, or"
    Write-Warn "    * create  $EnvPath  containing the single line:"
    Write-Warn "        OPENROUTER_API_KEY=sk-or-..."
    Write-Warn "  Re-run this installer with -ApiKey <key> to have it written for you."
}

# ---------------------------------------------------------------------------
# 3. autostart
# ---------------------------------------------------------------------------
Write-Step "Step 3/4  register autostart"
# Delegate to the daemon's own module so this value and the tray's
# "Start at login" toggle can never drift apart. It applies the same WSL/UNC
# refusal internally, and picks the base interpreter's pythonw.exe.
& $venvPython -c "import sys; sys.path.insert(0, sys.argv[1]); import autostart_windows as a; a.enable(sys.argv[2])" $AppDir $TrayScript
if ($LASTEXITCODE -ne 0) { Fail "autostart registration failed" }
Write-Ok ((Get-ItemProperty -Path $RunKeyPath -Name $RunValueName).$RunValueName)

# ---------------------------------------------------------------------------
# 4. launch
# ---------------------------------------------------------------------------
Write-Step "Step 4/4  launch tray"
if ($NoStart) {
    Write-Warn "-NoStart: not launching"
}
else {
    Start-Process -FilePath $basePythonw -ArgumentList $TrayScript -WorkingDirectory $AppDir
    Start-Sleep -Seconds 6
    $running = @(Get-DaemonProcess)
    if ($running.Count -ge 1) {
        Write-Ok "tray running (PID $($running[0].ProcessId))"
    }
    else {
        Write-Warn "no tray process detected yet -- check $InstallRoot\meter.log"
    }
}

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host "  runtime  : $AppDir"
Write-Host "  config   : $ConfigPath"
Write-Host "  api key  : $EnvPath"
Write-Host "  log      : $InstallRoot\meter.log"
Write-Host "  autostart: HKCU\...\Run\$RunValueName"
