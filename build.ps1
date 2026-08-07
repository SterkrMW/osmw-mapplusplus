#Requires -Version 5.1
<#
.SYNOPSIS
    Build variant releases of mapsplusplus.exe.

.DESCRIPTION
    For each variant manifest in variants\, writes a curated _addons.ahk and
    invokes Ahk2Exe to produce releases\<variant>\mapsplusplus.exe along with
    runtime assets (marker.png, maps\).

.PARAMETER Variant
    Build a single variant (matches variants\<name>.txt). If omitted, builds
    every manifest found in variants\.

.PARAMETER Ahk2ExePath
    Override the Ahk2Exe compiler location. If omitted, probes $env:AHK2EXE
    then the common AutoHotkey install paths.

.PARAMETER Base
    Optional path to the AutoHotkey runtime base .exe (e.g. AutoHotkey64.exe).
    If omitted, Ahk2Exe picks its default.

.PARAMETER Clean
    Wipe releases\ before building.

.EXAMPLE
    pwsh ./build.ps1 -Variant full

.EXAMPLE
    pwsh ./build.ps1            # builds every variant
#>
[CmdletBinding()]
param(
    [string] $Variant,
    [string] $Ahk2ExePath,
    [string] $Base,
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'

$RepoRoot      = $PSScriptRoot
$AddonsDir     = Join-Path $RepoRoot 'addons'
$VariantsDir   = Join-Path $RepoRoot 'variants'
$ReleasesDir   = Join-Path $RepoRoot 'releases'
$MainScript    = Join-Path $RepoRoot 'main.ahk'
$AddonsInclude = Join-Path $RepoRoot '_addons.ahk'
$MarkerPng     = Join-Path $RepoRoot 'marker.png'
$MapDir        = Join-Path $RepoRoot 'maps'
$UiDir         = Join-Path $RepoRoot 'ui'
$LibDir        = Join-Path $RepoRoot 'Lib'
$AvatarDir     = Join-Path $RepoRoot 'avatars'

# The version lives in variables.ahk; main.ahk carries a matching
# ;@Ahk2Exe-SetVersion literal so the compiled exe's file properties agree.
# Ahk2Exe cannot read the constant, so the two are checked against each other
# here — a silent drift would ship an exe whose properties disagree with what
# the app reports about itself, which is exactly what breaks bug triage.
function Get-AppVersion {
    $varsPath = Join-Path $RepoRoot 'variables.ahk'
    $mainPath = Join-Path $RepoRoot 'main.ahk'

    $varsText = Get-Content -LiteralPath $varsPath -Raw
    if ($varsText -notmatch '(?m)^\s*global\s+APP_VERSION\s*:=\s*"([^"]+)"') {
        throw "APP_VERSION not found in variables.ahk."
    }
    $version = $Matches[1]

    $mainText = Get-Content -LiteralPath $mainPath -Raw
    if ($mainText -notmatch '(?m)^\s*;@Ahk2Exe-SetVersion\s+(\S+)\s*$') {
        throw "No ;@Ahk2Exe-SetVersion directive found in main.ahk."
    }
    $directive = $Matches[1]

    if ($directive -ne $version) {
        throw ("Version mismatch: variables.ahk APP_VERSION is '$version' but " +
               "main.ahk ;@Ahk2Exe-SetVersion is '$directive'. Update both.")
    }
    return $version
}

function Resolve-Ahk2Exe {
    param([string] $Override)

    $candidates = @()
    if ($Override)        { $candidates += $Override }
    if ($env:AHK2EXE)     { $candidates += $env:AHK2EXE }
    $candidates += @(
        'C:\Program Files\AutoHotkey\Compiler\Ahk2Exe.exe',
        'C:\Program Files\AutoHotkey\v2\Compiler\Ahk2Exe.exe',
        'C:\Program Files (x86)\AutoHotkey\Compiler\Ahk2Exe.exe'
    )

    foreach ($p in $candidates) {
        if ($p -and (Test-Path -LiteralPath $p)) { return (Resolve-Path -LiteralPath $p).Path }
    }

    $tried = ($candidates | Where-Object { $_ }) -join "`n  "
    throw "Ahk2Exe.exe not found. Tried:`n  $tried`nPass -Ahk2ExePath or set `$env:AHK2EXE."
}

function Read-Manifest {
    param([string] $Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Manifest not found: $Path"
    }

    $addons = @()
    foreach ($raw in Get-Content -LiteralPath $Path) {
        $line = $raw.Trim()
        if (-not $line) { continue }
        if ($line.StartsWith('#')) { continue }
        $addons += $line
    }
    return ,$addons
}

function Test-Manifest {
    param(
        [string]   $VariantName,
        [string[]] $Addons
    )

    if ($Addons.Count -eq 0) {
        throw "Variant '$VariantName' lists no addons."
    }

    $missing = @()
    foreach ($a in $Addons) {
        if (-not (Test-Path -LiteralPath (Join-Path $AddonsDir $a))) {
            $missing += $a
        }
    }
    if ($missing.Count -gt 0) {
        $list = ($missing | ForEach-Object { "  addons\$_" }) -join "`n"
        throw "Variant '$VariantName' references missing addon files:`n$list"
    }
}

function Write-AddonsInclude {
    param([string[]] $Addons)

    $sb = New-Object System.Text.StringBuilder
    foreach ($a in $Addons) {
        [void]$sb.AppendLine("#Include addons\$a")
    }
    Set-Content -LiteralPath $AddonsInclude -Value $sb.ToString() -Encoding UTF8 -NoNewline
}

function Build-Variant {
    param(
        [string] $Name,
        [string] $ManifestPath,
        [string] $Compiler,
        [string] $Version
    )

    Write-Host ""
    Write-Host "=== Building variant: $Name (v$Version) ===" -ForegroundColor Cyan

    $addons = Read-Manifest -Path $ManifestPath
    Test-Manifest -VariantName $Name -Addons $addons

    Write-Host "  Addons:" -ForegroundColor DarkGray
    foreach ($a in $addons) { Write-Host "    - $a" -ForegroundColor DarkGray }

    # Snapshot existing _addons.ahk so dev mode keeps working after the build.
    $hadSnapshot = Test-Path -LiteralPath $AddonsInclude
    $snapshot    = if ($hadSnapshot) { Get-Content -LiteralPath $AddonsInclude -Raw } else { $null }

    $outDir = Join-Path $ReleasesDir $Name
    $outExe = Join-Path $outDir 'mapsplusplus.exe'

    try {
        Write-AddonsInclude -Addons $addons

        if (Test-Path -LiteralPath $outDir) {
            Remove-Item -LiteralPath $outDir -Recurse -Force
        }
        New-Item -ItemType Directory -Path $outDir -Force | Out-Null

        $compilerArgs = @('/in', $MainScript, '/out', $outExe)
        if ($Base) { $compilerArgs += @('/base', $Base) }

        Write-Host "  Compiling -> $outExe" -ForegroundColor DarkGray
        # Ahk2Exe is a GUI-subsystem exe; the call operator doesn't wait for it
        # and $LASTEXITCODE is unreliable. Start-Process -Wait blocks until exit.
        $proc = Start-Process -FilePath $Compiler -ArgumentList $compilerArgs `
                              -Wait -NoNewWindow -PassThru
        $compilerExit = $proc.ExitCode

        if (-not (Test-Path -LiteralPath $outExe)) {
            throw "Compile failed for variant '$Name' (Ahk2Exe exit=$compilerExit): output not created."
        }
        if ($compilerExit -ne 0) {
            Write-Warning "Ahk2Exe returned exit code $compilerExit but produced an output. Continuing."
        }

        # Copy runtime assets.
        if (Test-Path -LiteralPath $MarkerPng) {
            Copy-Item -LiteralPath $MarkerPng -Destination $outDir -Force
        } else {
            Write-Warning "marker.png missing at $MarkerPng -- shipped exe will warn at startup."
        }
        if (Test-Path -LiteralPath $MapDir) {
            Copy-Item -LiteralPath $MapDir -Destination $outDir -Recurse -Force
        } else {
            Write-Warning "maps\ folder missing at $MapDir -- shipped exe will warn at startup."
        }
        if (Test-Path -LiteralPath $UiDir) {
            Copy-Item -LiteralPath $UiDir -Destination $outDir -Recurse -Force
        } else {
            Write-Warning "ui\ folder missing at $UiDir -- settings window will be unavailable."
        }
        if (Test-Path -LiteralPath $AvatarDir) {
            Copy-Item -LiteralPath $AvatarDir -Destination $outDir -Recurse -Force
        } else {
            Write-Warning "avatars\ folder missing at $AvatarDir -- the radial menu will fall back to initials."
        }
        if (Test-Path -LiteralPath $LibDir) {
            Copy-Item -LiteralPath $LibDir -Destination $outDir -Recurse -Force
        }

        # Drop a small README naming the variant + its addons.
        $readme = @()
        $readme += "mapsplusplus.exe -- variant: $Name"
        $readme += "Version: $Version"
        $readme += "Built: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
        $readme += ""
        $readme += "Bundled addons:"
        foreach ($a in $addons) { $readme += "  - $a" }
        Set-Content -LiteralPath (Join-Path $outDir 'README.txt') -Value $readme -Encoding UTF8

        Write-Host "  OK: $outExe" -ForegroundColor Green
    }
    finally {
        # Restore the pre-build _addons.ahk state so running main.ahk uncompiled
        # afterwards still behaves like a normal dev environment.
        if ($hadSnapshot) {
            Set-Content -LiteralPath $AddonsInclude -Value $snapshot -Encoding UTF8 -NoNewline
        } elseif (Test-Path -LiteralPath $AddonsInclude) {
            Remove-Item -LiteralPath $AddonsInclude -Force
        }
    }
}

# --- Main ----------------------------------------------------------------

if (-not (Test-Path -LiteralPath $MainScript)) {
    throw "main.ahk not found at $MainScript"
}
if (-not (Test-Path -LiteralPath $VariantsDir)) {
    throw "variants\ folder not found at $VariantsDir"
}

$appVersion = Get-AppVersion
$compiler = Resolve-Ahk2Exe -Override $Ahk2ExePath
Write-Host "Maps++ version: $appVersion" -ForegroundColor DarkGray
Write-Host "Using compiler: $compiler" -ForegroundColor DarkGray

if ($Clean -and (Test-Path -LiteralPath $ReleasesDir)) {
    Write-Host "Cleaning $ReleasesDir" -ForegroundColor DarkGray
    Remove-Item -LiteralPath $ReleasesDir -Recurse -Force
}

if ($Variant) {
    $manifest = Join-Path $VariantsDir "$Variant.txt"
    Build-Variant -Name $Variant -ManifestPath $manifest -Compiler $compiler -Version $appVersion
} else {
    $manifests = Get-ChildItem -LiteralPath $VariantsDir -Filter '*.txt' -File
    if ($manifests.Count -eq 0) {
        throw "No variant manifests found in $VariantsDir"
    }
    foreach ($m in $manifests) {
        $name = [System.IO.Path]::GetFileNameWithoutExtension($m.Name)
        Build-Variant -Name $name -ManifestPath $m.FullName -Compiler $compiler -Version $appVersion
    }
}

Write-Host ""
Write-Host "Done." -ForegroundColor Green
