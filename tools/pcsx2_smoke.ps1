# Checks the disc image the build made and boots the game from it in PCSX2 up to the Main Menu (title -> autosave notice -> menu).
#   powershell -File tools\pcsx2_smoke.ps1 -Pcsx2 "C:\path\to\pcsx2-qt.exe"
# Two checks, exit code 0 = both pass:
#   1. output\ManiaPS2\SLUS_999.01.SonicManiaPS2.iso is mounted read-only in Windows and its contents are listed (SYSTEM.CNF, the boot file, DATA.RSDK).
#   2. A test copy of the disc is made from the same ELF and Data.rsdk plus a BOOT.TXT with scripted button presses (START at the title, CROSS on the
#      autosave notice), because the real disc has no script. It is started in PCSX2 as a disc image, and the emulator's console log must show
#      "cdrom0:" loading, Data.rsdk read from the disc, and the Main Menu.
# PCSX2 needs a BIOS set up and a memory card in slot 1 (the game keeps Settings.ini and saves there). The console log is read from
# <PCSX2 folder>\portable\logs\emulog.txt or Documents\PCSX2\logs\emulog.txt (or -Log): EE console output must be enabled in PCSX2's log settings.
param(
    [Parameter(Mandatory = $true)][string]$Pcsx2,
    [string]$Dir = '',
    [string]$Log = '',
    [int]$TimeoutSeconds = 170
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path $here -Parent
if (-not $Dir) { $Dir = Join-Path $root 'output\ManiaPS2' }
$iso = Join-Path $Dir 'SLUS_999.01.SonicManiaPS2.iso'
if (-not (Test-Path $Pcsx2)) { Write-Host "pcsx2-qt.exe not found: $Pcsx2" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $iso)) { Write-Host "Build first: $iso does not exist." -ForegroundColor Red; exit 1 }

# ---- 1. the real disc image
$ok = $true
try {
    $m = Mount-DiskImage -ImagePath $iso -PassThru -ErrorAction Stop
    Start-Sleep -Seconds 2
    $dl = ($m | Get-Volume).DriveLetter
    $files = @(Get-ChildItem "${dl}:\" | ForEach-Object { $_.Name })
    $cnf = Get-Content "${dl}:\SYSTEM.CNF" -Raw
    $boot = if ($cnf -match 'BOOT2\s*=\s*cdrom0:\\([^;\s]+);1') { $Matches[1] } else { '' }
    Write-Host ("Disc image: {0}; files: {1}" -f (Split-Path $iso -Leaf), ($files -join ', '))
    if (-not $boot -or $files -notcontains $boot) { Write-Host "FAIL: SYSTEM.CNF names a boot file that is not on the disc ('$boot')" -ForegroundColor Red; $ok = $false }
    if ($files -notcontains 'DATA.RSDK') { Write-Host "FAIL: DATA.RSDK is not on the disc" -ForegroundColor Red; $ok = $false }
}
catch { Write-Host "FAIL: the disc image could not be mounted: $($_.Exception.Message)" -ForegroundColor Red; $ok = $false }
finally { Dismount-DiskImage -ImagePath $iso -ErrorAction SilentlyContinue | Out-Null }
if (-not $ok) { exit 1 }

# ---- 2. boot a test copy in PCSX2
$elf  = Join-Path $root 'work\build\prototypes\p2_engine\p2_engine.elf'
$data = Join-Path $root 'input\Data.rsdk'
if (-not (Test-Path $elf) -or -not (Test-Path $data)) { Write-Host "Cannot make the test disc: work\build ELF or input\Data.rsdk is missing (run build.bat first)." -ForegroundColor Red; exit 1 }
if (-not $Log) {
    $cands = @((Join-Path (Split-Path $Pcsx2) 'portable\logs\emulog.txt'), (Join-Path (Split-Path $Pcsx2) 'logs\emulog.txt'), (Join-Path $env:USERPROFILE 'Documents\PCSX2\logs\emulog.txt'))
    $Log = $cands | Where-Object { Test-Path (Split-Path $_) } | Select-Object -First 1
}
if (-not $Log) { Write-Host "Cannot find PCSX2's log folder. Pass -Log <path to emulog.txt>." -ForegroundColor Red; exit 1 }

$tmp = Join-Path $env:TEMP ('maniaps2_smoke_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null
$boot = Join-Path $tmp 'boot.txt'
$testIso = Join-Path $tmp 'smoke.iso'
Set-Content -Path $boot -Value 'inject=1400:START:6/2000:CROSS:6' -Encoding ASCII -NoNewline
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here 'make_iso.ps1') -Elf $elf -Data $data -Out $testIso -BootTxt $boot | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "Cannot make the test disc." -ForegroundColor Red; exit 1 }

Get-Process pcsx2-qt -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Remove-Item $Log -ErrorAction SilentlyContinue
$proc = Start-Process -FilePath $Pcsx2 -ArgumentList @('-batch', '--', $testIso) -WorkingDirectory (Split-Path $Pcsx2) -PassThru
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$reached = $false; $fatal = $null; $lines = @()
try {
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        if (-not (Test-Path $Log)) { continue }
        try {
            $fs = [IO.File]::Open($Log, 'Open', 'Read', 'ReadWrite')
            $sr = New-Object IO.StreamReader($fs); $lines = $sr.ReadToEnd() -split "`n"; $sr.Dispose(); $fs.Dispose()
        } catch { continue }
        $fatal = $lines | Where-Object { $_ -match 'FATAL|Out of SFX storage|exception|bus error' } | Select-Object -First 1
        if ($fatal) { break }
        if ($lines | Where-Object { $_ -match 'Main Menu\s*$' }) { $reached = $true; break }
    }
} finally {
    Get-Process pcsx2-qt -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 800
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
$lines | Where-Object { $_ -match 'cdrom0|disc boot|Loaded file|Loading Scene' } | Select-Object -Last 6 | ForEach-Object { Write-Host "  $($_.Trim())" }
$fromDisc = [bool]($lines | Where-Object { $_ -match 'Loaded file cdrom0:DATA\.RSDK;1' })
if ($fatal) { Write-Host "FAIL: $fatal" -ForegroundColor Red; exit 1 }
if ($reached -and -not $fromDisc) { Write-Host "FAIL: reached the Main Menu but Data.rsdk was not read from the disc" -ForegroundColor Red; exit 1 }
if ($reached) { Write-Host "PASS: the disc image is valid and boots to the Main Menu" -ForegroundColor Green; exit 0 }
Write-Host "FAIL: the Main Menu was not reached within $TimeoutSeconds s" -ForegroundColor Red
exit 1
