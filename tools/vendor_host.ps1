<#
.SYNOPSIS
Copies the three binaries TpF2 Big Maps SHARES with TpF2 Multiplayer into
installer\vendor\ and records where they came from.

.DESCRIPTION
The proxy (alut.dll), the plugin host (tpf2_pluginhost.dll) and the installer
custom actions (tpf2ca.dll) are built in the tpf2-multiplayer repository. Both
packages must ship the SAME bytes under the SAME component GUIDs (PluginHost.wxs)
so Windows Installer can treat them as one shared component and the two products
can be installed and removed in any order. So this repository never builds them;
it vendors them from a sibling checkout and writes vendor\VENDORED.md with the
source commit, whether that tree was dirty, and a SHA-256 per file.

It also checks that installer\PluginHost.wxs is byte-identical to the
tpf2-multiplayer copy, because a drift there means the two packages disagree
about a GUID -- the exact failure the arrangement exists to prevent.

.PARAMETER MpRepo
Path to the tpf2-multiplayer checkout. Defaults to ..\tpf2-multiplayer beside
this repository.

.PARAMETER Build
Run bridge\build_host.bat and installer\ca\build_ca.bat in that checkout first.
Close the game: a loaded DLL cannot be relinked (LNK1104).

.EXAMPLE
powershell -File tools\vendor_host.ps1 -Build
#>
[CmdletBinding()]
param(
    [string]$MpRepo = "",
    [switch]$Build
)
$ErrorActionPreference = "Stop"
# $PSScriptRoot is not populated inside param() defaults on PowerShell 5.1
# when run with -File, so resolve paths here instead.
$Here   = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$Repo   = Split-Path -Parent $Here
if (-not $MpRepo) { $MpRepo = Join-Path (Split-Path -Parent $Repo) "tpf2-multiplayer" }
$Vendor = Join-Path $Repo "installer\vendor"
function Say($m, $c = "Cyan") { Write-Host "[vendor] $m" -ForegroundColor $c }
function Fail($m) { Write-Host "[vendor] $m" -ForegroundColor Red; exit 1 }

if (-not (Test-Path (Join-Path $MpRepo "bridge\build_host.bat"))) { Fail "not a tpf2-multiplayer checkout: $MpRepo" }

if ($Build) {
    foreach ($bat in @("bridge\build_host.bat", "installer\ca\build_ca.bat")) {
        Say "running $bat"
        cmd /c "`"$(Join-Path $MpRepo $bat)`" 2>&1" | Where-Object { "$_" -notmatch "vswhere|operable program or batch file" } | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) { Fail "$bat failed (exit $LASTEXITCODE). If it was LNK1104, close the game." }
    }
}

$sources = @{
    "alut.dll"            = Join-Path $MpRepo "bridge\out\alut.dll"
    "tpf2_pluginhost.dll" = Join-Path $MpRepo "bridge\out\tpf2_pluginhost.dll"
    "tpf2ca.dll"          = Join-Path $MpRepo "installer\out\tpf2ca.dll"
}
foreach ($k in $sources.Keys) { if (-not (Test-Path $sources[$k])) { Fail "missing $($sources[$k]) -- build it there, or pass -Build" } }

# The shared fragment must match exactly.
$mine   = Join-Path $Repo "installer\PluginHost.wxs"
$theirs = Join-Path $MpRepo "installer\PluginHost.wxs"
if (-not (Test-Path $theirs)) { Fail "no installer\PluginHost.wxs in $MpRepo" }
if ((Get-FileHash $mine).Hash -ne (Get-FileHash $theirs).Hash) {
    Fail "installer\PluginHost.wxs differs from the tpf2-multiplayer copy. Copy one over the other; they must be byte-identical."
}
Say "PluginHost.wxs matches"

New-Item -ItemType Directory -Force $Vendor | Out-Null
$commit = (git -C $MpRepo rev-parse HEAD).Trim()
$short  = (git -C $MpRepo rev-parse --short HEAD).Trim()
$dirty  = [bool](git -C $MpRepo status --porcelain -- bridge/src installer/ca)
$branch = (git -C $MpRepo rev-parse --abbrev-ref HEAD).Trim()

$lines = @(
    "# Vendored shared binaries",
    "",
    "These three files are built in the tpf2-multiplayer repository and copied here",
    "unchanged. Both packages ship them under the SAME component GUIDs (see",
    "PluginHost.wxs), so they must be the same bytes. Regenerate with",
    "``tools\vendor_host.ps1``; never edit or rebuild them here.",
    "",
    "source repo:   https://github.com/silver2127/tpf2-multiplayer",
    "source commit: $commit ($short, branch $branch)",
    ("source tree:   " + $(if ($dirty) { "DIRTY in bridge/src or installer/ca -- the binaries may not match that commit exactly" } else { "clean" })),
    "vendored on:   $(Get-Date -Format 'yyyy-MM-dd HH:mm')",
    "",
    "| file | bytes | sha256 |",
    "| --- | --- | --- |"
)
foreach ($k in ($sources.Keys | Sort-Object)) {
    Copy-Item $sources[$k] (Join-Path $Vendor $k) -Force
    $f = Get-Item (Join-Path $Vendor $k)
    $lines += "| $k | $($f.Length) | $((Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower()) |"
    Say "vendored $k ($($f.Length) bytes)"
}
Set-Content (Join-Path $Vendor "VENDORED.md") ($lines -join "`n") -Encoding utf8
Say "wrote vendor\VENDORED.md (source $short$(if ($dirty) { ', DIRTY' }))" $(if ($dirty) { "Yellow" } else { "Green" })
