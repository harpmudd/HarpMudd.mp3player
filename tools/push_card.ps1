<#
Copy dist\ to the Pocket's SD card and verify it, or fail loudly.

Written after the ad-hoc version reported "card matches dist" for a push where
the card had been removed mid-copy and NOTHING was written. Every file's
Join-Path threw, $dst came back null, Test-Path errored instead of returning
false, so the mismatch counter stayed at zero and the check passed itself.

The rules that keeps it honest:
  - resolve the card ONCE and re-check it right before verifying, so a card
    that disappears is an error rather than a silent zero
  - count files COMPARED, not just mismatches; a run that compared nothing is
    a failure even with no mismatches
  - -ErrorAction Stop inside a try, so a vanished drive cannot be shrugged off
#>
[CmdletBinding()]
param([string]$Dist = "$PSScriptRoot\..\dist")

$ErrorActionPreference = 'Stop'
$Dist = (Resolve-Path $Dist).Path

$card = $null
foreach ($d in (Get-PSDrive -PSProvider FileSystem).Root) {
    if (Test-Path (Join-Path $d 'Assets\mp3player\common')) { $card = $d; break }
}
if (-not $card) { Write-Error 'card not mounted'; exit 1 }
Write-Host "card: $card"

$files = @(Get-ChildItem $Dist -Recurse -File)
if ($files.Count -eq 0) { Write-Error "dist is empty: $Dist"; exit 1 }

$copied = 0
foreach ($f in $files) {
    $rel = $f.FullName.Substring($Dist.Length + 1)
    $dst = Join-Path $card $rel
    $need = $true
    if (Test-Path $dst) {
        $need = (Get-FileHash $f.FullName -Algorithm SHA256).Hash -ne
                (Get-FileHash $dst        -Algorithm SHA256).Hash
    }
    if ($need) {
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        Copy-Item $f.FullName $dst -Force
        Write-Host ("  copied  {0}" -f $rel)
        $copied++
    }
}

# Re-check the card is still there before trusting anything the verify says.
if (-not (Test-Path (Join-Path $card 'Assets\mp3player\common'))) {
    Write-Error 'card disappeared during the copy -- nothing is verified'
    exit 1
}

$compared = 0
$bad = @()
foreach ($f in $files) {
    $rel = $f.FullName.Substring($Dist.Length + 1)
    $dst = Join-Path $card $rel
    try {
        if (-not (Test-Path $dst)) { $bad += "missing: $rel"; continue }
        $a = (Get-FileHash $f.FullName -Algorithm SHA256).Hash
        $b = (Get-FileHash $dst        -Algorithm SHA256).Hash
        if ($a -ne $b) { $bad += "differs: $rel" }
        $compared++
    } catch {
        $bad += ("unreadable: {0} ({1})" -f $rel, $_.Exception.Message)
    }
}

if ($compared -ne $files.Count) {
    Write-Error ("only {0} of {1} files could be compared -- NOT verified" -f $compared, $files.Count)
    $bad | ForEach-Object { Write-Host "  $_" }
    exit 1
}
if ($bad.Count) {
    Write-Error ("{0} file(s) wrong on card" -f $bad.Count)
    $bad | ForEach-Object { Write-Host "  $_" }
    exit 1
}

Write-Host ("  {0} copied, {1}/{2} verified identical" -f $copied, $compared, $files.Count)
$rom = Join-Path $card 'Assets\mp3player\common\mp3player.rom'
Write-Host ("  firmware {0:N0} bytes" -f (Get-Item $rom).Length)
