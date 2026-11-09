# Fails if the release tree contains anything that must not be published: game data, builds, media, saves, or files that look like Sonic Mania data.
#   powershell -File tools\check_release_tree.ps1            (from the release folder; run before every commit / release)
# input\, output\ and work\ are skipped: they are git-ignored and hold the user's own files.
param([string]$Root = '')
if (-not $Root) { $Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path) }
$bad = @()
$skipDirs = @('input', 'output', 'work', '.git')
$forbidden = '\.(rsdk|elf|iso|img|irx|o|map|exe|dll|so|gif|png|jpe?g|bmp|wav|ogg|mp3|mp4|ogv|webm|zip|7z|rar|gz|bin)$'
$allowedNames = @()
Get-ChildItem -LiteralPath $Root -Recurse -File -Force | ForEach-Object {
    $rel = $_.FullName.Substring($Root.Length).TrimStart('\')
    $top = $rel.Split('\')[0]
    if ($skipDirs -contains $top -and $rel -ne 'input\README.txt') { return }
    if ($rel -eq 'input\README.txt') { return }
    if ($_.Name -match $forbidden -and $allowedNames -notcontains $_.Name) { $bad += "forbidden file type: $rel"; return }
    if ($_.Name -match '^(Settings\.ini|SaveData\.bin|Achievements\.bin|log\.txt|emulog\.txt|boot\.txt)$') { $bad += "save/settings/log file: $rel"; return }
    if ($_.Length -gt 1MB) { $bad += ("larger than 1 MB: {0} ({1:N0} bytes)" -f $rel, $_.Length); return }
    # the magic of an RSDK data pack, wherever it hides
    $fs = [IO.File]::OpenRead($_.FullName)
    try { $h = New-Object byte[] 8; $n = $fs.Read($h, 0, 8) } finally { $fs.Dispose() }
    if ($n -ge 6 -and [Text.Encoding]::ASCII.GetString($h, 0, 6) -eq 'RSDKv5') { $bad += "looks like an RSDK data file: $rel" }
    # the magic of the kinds of file the game data and the builds are made of, whatever the file is called
    if ($n -ge 4) {
        $m4 = [Text.Encoding]::ASCII.GetString($h, 0, 4)
        if ($m4 -eq 'OggS' -or $m4 -eq 'RIFF' -or $m4 -eq 'GIF8' -or $m4 -eq "SPR`0" -or $m4 -eq ([string][char]0x7F + 'ELF') -or ($h[0] -eq 0x89 -and $m4.Substring(1) -eq 'PNG')) {
            $bad += "looks like game media or a build (magic $($m4 -replace '[^\x20-\x7E]', '?')): $rel"
        }
    }
}
if ($bad.Count) {
    Write-Host "RELEASE TREE CHECK FAILED:" -ForegroundColor Red
    $bad | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
$count = (Get-ChildItem -LiteralPath $Root -Recurse -File -Force | Where-Object { $_.FullName.Substring($Root.Length).TrimStart('\').Split('\')[0] -notin $skipDirs }).Count
Write-Host "Release tree check passed ($count files scanned; input, output and work are git-ignored and skipped)." -ForegroundColor Green
exit 0
