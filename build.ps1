# Builds the Sonic Mania PS2 port from the user's own Data.rsdk.
#
#   build.bat                      (double-click, or run from a terminal)
#   powershell -File build.ps1 [-Refetch] [-SkipHashCheck] [-KeepElf]
#
# What it does: checks Docker and input\Data.rsdk, builds the pinned ps2dev container, downloads the upstream decompilation sources at their pinned commits,
# applies this repository's patches, compiles for the PS2, and writes a PS2 disc image (output\ManiaPS2\SLUS_999.01.SonicManiaPS2.iso) holding the game and your Data.rsdk.
# Nothing is read from or written outside this folder except Docker's own image storage.
[CmdletBinding()]
param(
    [switch]$Refetch,        # delete the cached upstream clone and download it again
    [switch]$SkipHashCheck,  # do not compare Data.rsdk with the list of tested versions
    [switch]$KeepElf         # also leave SonicManiaPS2.elf and a copy of Data.rsdk in the output folder (launching from USB with a homebrew launcher)
)
$ErrorActionPreference = 'Stop'
$root   = $PSScriptRoot
$inputDir = Join-Path $root 'input'
$work   = Join-Path $root 'work'
$stage  = Join-Path $work 'stage'
$output = Join-Path $root 'output\ManiaPS2'
$image  = 'maniaps2-release-ps2dev'
$dataFile = Join-Path $inputDir 'Data.rsdk'

function Step([string]$m) { Write-Host ""; Write-Host "==> $m" -ForegroundColor Cyan }
function Fail([string]$m) { Write-Host ""; Write-Host "ERROR: $m" -ForegroundColor Red; exit 1 }
function Warn([string]$m) { Write-Host "WARNING: $m" -ForegroundColor Yellow }

Write-Host "Sonic Mania PS2 port - build" -ForegroundColor Green
Write-Host "Non-commercial, for personal use with your own legally obtained copy of the game. See README.md." -ForegroundColor DarkGray

# ---------------------------------------------------------------- 1. your game data
Step "Checking your Data.rsdk"
if (-not (Test-Path -LiteralPath $dataFile)) {
    Fail ("input\Data.rsdk was not found.`n" +
          "Copy the Data.rsdk from your own installed copy of Sonic Mania Plus into:`n  $inputDir`n" +
          "(The Steam version keeps it in the game folder, next to SonicMania.exe.) This repository cannot and does not include it.")
}
$info = Get-Item -LiteralPath $dataFile
$fs = [IO.File]::OpenRead($dataFile)
try { $head = New-Object byte[] 6; [void]$fs.Read($head, 0, 6) } finally { $fs.Dispose() }
if ([Text.Encoding]::ASCII.GetString($head) -ne 'RSDKv5') { Fail "input\Data.rsdk is not an RSDKv5 data file (wrong header). Use the Data.rsdk from Sonic Mania Plus." }
if ($info.Length -lt 50MB) { Fail ("input\Data.rsdk is only {0:N0} bytes: too small to be the full game data. Is the copy complete?" -f $info.Length) }
Write-Host ("Found Data.rsdk, {0:N0} bytes" -f $info.Length)
if (-not $SkipHashCheck) {
    $hash = (Get-FileHash -LiteralPath $dataFile -Algorithm SHA256).Hash.ToUpperInvariant()
    $known = @{}
    Get-Content (Join-Path $root 'data-hashes.txt') | Where-Object { $_ -match '^[0-9A-Fa-f]{64}\s' } | ForEach-Object {
        $p = $_ -split '\s+', 2; $known[$p[0].ToUpperInvariant()] = $p[1]
    }
    if ($known.ContainsKey($hash)) { Write-Host ("Tested version: " + $known[$hash]) -ForegroundColor Green }
    else { Warn "This Data.rsdk is not one of the tested versions (SHA-256 $hash). The build continues, but the game may misbehave. Only the Steam PC version of Sonic Mania Plus has been tested." }
}

# ---------------------------------------------------------------- 2. docker
Step "Checking Docker"
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { Fail "Docker was not found. Install Docker Desktop (https://www.docker.com/products/docker-desktop/), start it, and run this again." }
$dockerUp = $true
try { & docker info *> $null; if ($LASTEXITCODE -ne 0) { $dockerUp = $false } } catch { $dockerUp = $false }
if (-not $dockerUp) { Fail "Docker is installed but not running. Start Docker Desktop, wait until it says it is running, and try again." }
Write-Host "Docker is running"

Step "Preparing the build container (the first run downloads about 1 GB)"
& docker build -t $image (Join-Path $root 'docker')
if ($LASTEXITCODE -ne 0) { Fail "The container could not be built. Check your internet connection and Docker's status." }

# ---------------------------------------------------------------- 3. stage our files (LF line endings: they run inside a Linux container)
Step "Staging this repository's patches and sources"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
$utf8 = New-Object Text.UTF8Encoding($false)
foreach ($dir in 'patches', 'src', 'ps2-build', 'docker') {
    $srcDir = Join-Path $root $dir
    Get-ChildItem $srcDir -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($root.Length).TrimStart('\')
        $dest = Join-Path $stage $rel
        New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
        if ($_.Name -eq 'Dockerfile') { Copy-Item $_.FullName $dest; return }
        $text = [IO.File]::ReadAllText($_.FullName) -replace "`r`n", "`n"
        [IO.File]::WriteAllText($dest, $text, $utf8)
    }
}
if ($Refetch -and (Test-Path (Join-Path $work 'cache'))) { Remove-Item (Join-Path $work 'cache') -Recurse -Force }
New-Item -ItemType Directory -Force $work | Out-Null

# ---------------------------------------------------------------- 4. fetch upstream + patch, then build
Step "Fetching the upstream decompilation sources and applying the patches"
& docker run --rm -v "${work}:/work" $image sh /work/stage/docker/container-prepare.sh
if ($LASTEXITCODE -ne 0) { Fail "Fetching or patching the sources failed (see above). Without internet access the first run cannot work." }

Step "Compiling for the PS2 (several minutes)"
& docker run --rm -v "${work}:/work" $image sh /work/stage/docker/container-build.sh
if ($LASTEXITCODE -ne 0) { Fail "The build failed (see above)." }

# ---------------------------------------------------------------- 5. package
Step "Making the disc image (ISO)"
$elf = Join-Path $work 'build\prototypes\p2_engine\p2_engine.elf'
if (-not (Test-Path $elf)) { Fail "The build reported success but no ELF exists." }
if (Test-Path $output) { Remove-Item $output -Recurse -Force }
New-Item -ItemType Directory -Force $output | Out-Null
$iso = Join-Path $output 'SLUS_999.01.SonicManiaPS2.iso'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\make_iso.ps1') -Elf $elf -Data $dataFile -Out $iso
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $iso)) { Fail "The disc image could not be made (see above)." }
if ($KeepElf) {
    Copy-Item $elf (Join-Path $output 'SonicManiaPS2.elf')
    Copy-Item -LiteralPath $dataFile (Join-Path $output 'Data.rsdk')
}
@"
Sonic Mania PS2 - built for personal use from your own copy of the game.

SLUS_999.01.SonicManiaPS2.iso is a PlayStation 2 disc image. It contains SEGA's game data and a build of SEGA's game code:
do NOT upload, share or sell it.

PS2: start it like any other game image, for example with Open PS2 Loader (OPL) from a USB stick, a network share
or the internal hard disk. The image is about 215 MB, so OPL lists it under CD. It needs a console that can run
game images (Free McBoot or a softmod with OPL).
The game reads its data from the disc image. Settings and saves go to the memory card in slot 1
(folder SONICMANIAPS2, the game never formats a card); without a card they go to a USB stick if one is plugged in.
PCSX2:     File > Start File, choose SLUS_999.01.SonicManiaPS2.iso.

See README.md in the repository for settings, saves and known issues.
"@ | Set-Content -Path (Join-Path $output 'README.txt') -Encoding ASCII

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host ("  ISO : {0}  ({1:N0} bytes)" -f $iso, (Get-Item $iso).Length)
if ($KeepElf) { Write-Host ("  ELF : {0}  (plus Data.rsdk next to it, for launching from a USB stick with a homebrew launcher)" -f (Join-Path $output 'SonicManiaPS2.elf')) }
Write-Host "Run the ISO with OPL on your PS2, or open it in PCSX2."
exit 0
