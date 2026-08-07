#Requires -Version 5.1
<#
.SYNOPSIS
    Flags local variables that shadow an AutoHotkey built-in.

.DESCRIPTION
    AHK v2 refuses to compile a script whose local variable shares a name with a
    built-in function or keyword, and Ahk2Exe reports it as a bare "Failed to
    compile" with no line number. The script still RUNS fine under
    AutoHotkey64.exe, so nothing points at the cause.

    This has broken the build four times (mod, local, run, max), each costing a
    bisect. Running it before a build turns a twenty-minute hunt into a line
    number. build.ps1 calls it automatically.

.EXAMPLE
    pwsh ./tools/check-shadowing.ps1
#>
[CmdletBinding()]
param(
    [string] $Root = (Split-Path $PSScriptRoot -Parent)
)

$ErrorActionPreference = 'Stop'

# Built-ins and keywords whose names are plausible as variables. Not the whole
# API — only names someone would reasonably reach for, since flagging e.g.
# WinGetPos as a variable name would be noise.
$reserved = @(
    'mod', 'local', 'run', 'max', 'min', 'abs', 'round', 'floor', 'ceil',
    'format', 'sort', 'type', 'send', 'click', 'sleep', 'random', 'chr', 'ord',
    'trim', 'strlen', 'substr', 'input', 'hotkey', 'menu', 'gui', 'func',
    'loop', 'while', 'until', 'if', 'else', 'return', 'break', 'continue',
    'global', 'static', 'throw', 'try', 'catch', 'finally', 'switch', 'case',
    'and', 'or', 'not', 'in', 'is', 'contains', 'true', 'false', 'unset'
)

$files = @()
$files += Get-ChildItem -LiteralPath $Root -Filter '*.ahk' -File |
          Where-Object { $_.Name -ne '_addons.ahk' }
$addons = Join-Path $Root 'addons'
if (Test-Path -LiteralPath $addons) {
    $files += Get-ChildItem -LiteralPath $addons -Filter '*.ahk' -File
}

$hits = @()
foreach ($f in $files) {
    $lineNo = 0
    foreach ($line in Get-Content -LiteralPath $f.FullName) {
        $lineNo++
        # Strip trailing comments so a name inside prose is not flagged. Naive
        # about ';' in string literals, which is acceptable for a lint.
        $code = ($line -split ';')[0]
        if ($code -match '^\s*([A-Za-z_]\w*)\s*:=') {
            $name = $Matches[1]
            if ($reserved -contains $name.ToLower()) {
                $hits += [pscustomobject]@{
                    File = $f.Name; Line = $lineNo; Name = $name; Text = $code.Trim()
                }
            }
        }
    }
}

if ($hits.Count -eq 0) {
    Write-Host "No locals shadow a built-in." -ForegroundColor DarkGray
    exit 0
}

Write-Host ""
Write-Host "Local variables shadowing an AutoHotkey built-in:" -ForegroundColor Red
foreach ($h in $hits) {
    Write-Host ("  {0}:{1}  '{2}'" -f $h.File, $h.Line, $h.Name) -ForegroundColor Yellow
    Write-Host ("      {0}" -f $h.Text) -ForegroundColor DarkGray
}
Write-Host ""
Write-Host "Rename these — Ahk2Exe will refuse to compile without saying why." -ForegroundColor Red
exit 1
