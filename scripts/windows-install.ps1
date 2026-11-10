# Install (or uninstall) a Windows build of mep under a prefix -- the
# Windows half of `just install` / `just uninstall`.
#
# Lays out PREFIX\bin (mep.exe and its helper programs) and
# PREFIX\share\mep (help pages, assets), the same tree the Unix install
# makes, and adds a Start Menu shortcut. Re-running replaces the previous
# install: every file the last install recorded in the build directory's
# install_manifest.txt is removed first, so files dropped from the build
# don't linger.
#
# Usage:
#   scripts\windows-install.ps1 -BuildDir build\win -Prefix C:\Users\me\.local
#   scripts\windows-install.ps1 -BuildDir build\win -Uninstall

param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$Prefix,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

# just hands cmd.exe the recipe line with its quotes escaped, so a quoted
# argument arrives here with the quote characters still in it.
$BuildDir = $BuildDir.Trim('"')
if ($Prefix) { $Prefix = $Prefix.Trim('"') }

$manifest = Join-Path $BuildDir 'install_manifest.txt'
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'mep.lnk'

function Normalize([string]$path) {
    return [IO.Path]::GetFullPath($path).TrimEnd('\')
}

# Removes the files the last install recorded (only those under $under,
# when given, so a manifest from a different prefix is left alone).
function Remove-Manifest([string]$under) {
    if (-not (Test-Path -LiteralPath $manifest)) { return 0 }
    $count = 0
    foreach ($line in Get-Content -LiteralPath $manifest) {
        if (-not $line) { continue }
        $file = Normalize $line
        if ($under -and -not $file.StartsWith($under + '\', [StringComparison]::OrdinalIgnoreCase)) { continue }
        if (Test-Path -LiteralPath $file) {
            Remove-Item -LiteralPath $file -Force
            $count++
        }
    }
    return $count
}

# Windows will not replace or delete a running .exe: refuse up front if
# one of the programs this build installs is running from $under\bin.
function Assert-NotRunning([string]$under) {
    $ours = @(Get-ChildItem -LiteralPath $BuildDir -Filter 'mep*.exe' -File | ForEach-Object { $_.Name })
    $bin = Join-Path $under 'bin'
    $running = Get-Process -ErrorAction SilentlyContinue | Where-Object {
        $path = $null
        try { $path = $_.Path } catch { }
        $path -and $ours -contains (Split-Path -Leaf $path) -and (Split-Path -Parent $path) -ieq $bin
    }
    if ($running) {
        $names = ($running | ForEach-Object { "$($_.Name) (pid $($_.Id))" }) -join ', '
        throw "mep is running from $under -- close it first: $names"
    }
}

if ($Uninstall) {
    if (-not (Test-Path -LiteralPath $manifest)) {
        Write-Host "nothing to uninstall (no $manifest)"
        exit 0
    }
    $bin = Get-Content -LiteralPath $manifest | Where-Object { $_ -match '[\\/]mep\.exe$' } | Select-Object -First 1
    if ($bin) { Assert-NotRunning (Split-Path (Split-Path (Normalize $bin))) }
    $n = Remove-Manifest $null
    Remove-Item -LiteralPath $manifest -Force
    if (Test-Path -LiteralPath $shortcut) { Remove-Item -LiteralPath $shortcut -Force }
    Write-Host "Removed $n files and the Start Menu shortcut"
    exit 0
}

if (-not $Prefix) { throw '-Prefix is required unless -Uninstall is given' }
$Prefix = Normalize $Prefix
$exe = Join-Path $Prefix 'bin\mep.exe'

Assert-NotRunning $Prefix
Remove-Manifest $Prefix | Out-Null

& cmake --install $BuildDir --prefix $Prefix
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# Start Menu entry, so mep can be launched like any other app.
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut($shortcut)
$lnk.TargetPath = $exe
$lnk.WorkingDirectory = [Environment]::GetFolderPath('UserProfile')
$lnk.Description = 'mep'
$lnk.Save()

Write-Host "Installed mep to $exe (Start Menu: mep)"

# The TeX bundle (`just tex-bundle`) is built with nix, which Windows does
# not have; without it mep-tex falls back to a TeX installation on PATH.
if (-not (Test-Path -LiteralPath (Join-Path $BuildDir 'tex-bundle.mtb'))) {
    Write-Host 'note: the TeX bundle is not installed; mep-tex will need a TeX installation on PATH'
}

$binDir = Join-Path $Prefix 'bin'
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$onPath = ("$userPath;$machinePath;$env:Path" -split ';') |
    Where-Object { $_ } |
    ForEach-Object { $entry = $_; try { Normalize ([Environment]::ExpandEnvironmentVariables($entry)) } catch { $entry } } |
    Where-Object { $_ -ieq $binDir }
if (-not $onPath) {
    Write-Host "note: $binDir is not on your PATH. To add it for your user:"
    Write-Host "  [Environment]::SetEnvironmentVariable('Path', `"$binDir;`" + [Environment]::GetEnvironmentVariable('Path', 'User'), 'User')"
}
