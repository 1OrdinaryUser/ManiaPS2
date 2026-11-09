# Builds the PlayStation 2 disc image (ISO 9660) the game is run from: SYSTEM.CNF + the game's ELF + the user's Data.rsdk.
#   powershell -File tools\make_iso.ps1 -Elf <SonicManiaPS2.elf> -Data <Data.rsdk> -Out <SonicManiaPS2.iso> [-BootTxt <file>] [-DiscId SLUS_999.01] [-Label SONIC_MANIA_PS2]
#
# What goes on the disc (all names are ISO 9660 level 1, upper case, flat root directory):
#   SYSTEM.CNF    BOOT2 = cdrom0:\<DiscId>;1   VER = 1.00   VMODE = NTSC
#   <DiscId>      the game (a plain ELF; the console's BIOS loads it from the disc)
#   DATA.RSDK     the user's own Data.rsdk (the game reads it from cdrom0:\DATA.RSDK;1)
#   BOOT.TXT      only with -BootTxt: developer test script (scripted input etc.), see docs\DEVELOPER_NOTES.md
# The image is written deterministically (fixed dates), so the same inputs give the same file.
# No third-party tool is needed: the small ISO 9660 writer is compiled from the C# code below.
param(
    [Parameter(Mandatory = $true)][string]$Elf,
    [string]$Data = '',   # omitted only for the console test disc (prototypes\p3_hello)
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$BootTxt = '',
    [string]$DiscId = 'SLUS_999.01',
    [string]$Label = 'SONIC_MANIA_PS2'
)
$ErrorActionPreference = 'Stop'

foreach ($f in @($Elf, $Data | Where-Object { $_ })) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { Write-Host "make_iso: file not found: $f" -ForegroundColor Red; exit 1 }
}
if ($BootTxt -and -not (Test-Path -LiteralPath $BootTxt -PathType Leaf)) { Write-Host "make_iso: file not found: $BootTxt" -ForegroundColor Red; exit 1 }
if ($DiscId -notmatch '^[A-Z]{4}_[0-9]{3}\.[0-9]{2}$') { Write-Host "make_iso: DiscId must look like SLUS_999.01" -ForegroundColor Red; exit 1 }
if ($Label -notmatch '^[A-Z0-9_]{1,32}$') { Write-Host "make_iso: Label may only contain A-Z, 0-9 and _ (32 characters at most)" -ForegroundColor Red; exit 1 }

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public static class Ps2IsoWriter
{
    const int Sector = 2048;

    public class Entry
    {
        public string Name;      // ISO name without version, e.g. "DATA.RSDK"
        public string Path;      // source file, or null when Bytes is used
        public byte[] Bytes;
        public long Length;
        public int Lba;
    }

    static void Both32(byte[] b, int o, uint v)
    {
        b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); b[o + 2] = (byte)(v >> 16); b[o + 3] = (byte)(v >> 24);
        b[o + 4] = (byte)(v >> 24); b[o + 5] = (byte)(v >> 16); b[o + 6] = (byte)(v >> 8); b[o + 7] = (byte)v;
    }
    static void Both16(byte[] b, int o, ushort v)
    {
        b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); b[o + 2] = (byte)(v >> 8); b[o + 3] = (byte)v;
    }
    static void Le32(byte[] b, int o, uint v) { b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); b[o + 2] = (byte)(v >> 16); b[o + 3] = (byte)(v >> 24); }
    static void Be32(byte[] b, int o, uint v) { b[o] = (byte)(v >> 24); b[o + 1] = (byte)(v >> 16); b[o + 2] = (byte)(v >> 8); b[o + 3] = (byte)v; }
    static void Text(byte[] b, int o, int len, string s)
    {
        for (int i = 0; i < len; i++) b[o + i] = (byte)(i < s.Length ? s[i] : ' ');
    }

    // fixed date 2026-01-01 00:00:00 (UTC): 7-byte directory form
    static void DirDate(byte[] b, int o) { b[o] = 126; b[o + 1] = 1; b[o + 2] = 1; b[o + 3] = 0; b[o + 4] = 0; b[o + 5] = 0; b[o + 6] = 0; }

    static int DirRecord(byte[] b, int o, int lba, uint len, bool isDir, byte[] id)
    {
        int recLen = 33 + id.Length;
        if ((recLen & 1) != 0) recLen++;
        b[o] = (byte)recLen;
        b[o + 1] = 0;
        Both32(b, o + 2, (uint)lba);
        Both32(b, o + 10, len);
        DirDate(b, o + 18);
        b[o + 25] = (byte)(isDir ? 2 : 0);
        b[o + 26] = 0; b[o + 27] = 0;
        Both16(b, o + 28, 1);
        b[o + 32] = (byte)id.Length;
        for (int i = 0; i < id.Length; i++) b[o + 33 + i] = id[i];
        return recLen;
    }

    public static void Write(string outPath, string label, List<Entry> files)
    {
        // layout: 0-15 system area, 16 PVD, 17 terminator, 18 L path table, 19 M path table, 20 root directory, then the files (sector aligned)
        files.Sort(delegate (Entry a, Entry c) { return string.CompareOrdinal(a.Name, c.Name); });
        int lba = 21;
        foreach (Entry e in files)
        {
            e.Length = e.Bytes != null ? e.Bytes.Length : new FileInfo(e.Path).Length;
            if (e.Length > 0xFFFFFFFFL) throw new Exception(e.Name + " is larger than 4 GB");
            e.Lba = lba;
            lba += (int)((e.Length + Sector - 1) / Sector);
        }
        int totalSectors = lba;

        byte[] dir = new byte[Sector];
        int p = 0;
        p += DirRecord(dir, p, 20, Sector, true, new byte[] { 0 });
        p += DirRecord(dir, p, 20, Sector, true, new byte[] { 1 });
        foreach (Entry e in files)
        {
            byte[] id = Encoding.ASCII.GetBytes(e.Name + ";1");
            int need = 33 + id.Length; if ((need & 1) != 0) need++;
            if (p + need > Sector) throw new Exception("root directory is full");
            p += DirRecord(dir, p, e.Lba, (uint)e.Length, false, id);
        }

        byte[] pvd = new byte[Sector];
        pvd[0] = 1; Text(pvd, 1, 5, "CD001"); pvd[6] = 1;
        Text(pvd, 8, 32, "PLAYSTATION");
        Text(pvd, 40, 32, label);
        Both32(pvd, 80, (uint)totalSectors);
        Both16(pvd, 120, 1);
        Both16(pvd, 124, 1);
        Both16(pvd, 128, Sector);
        Both32(pvd, 132, 10);
        Le32(pvd, 140, 18);
        Be32(pvd, 148, 19);
        DirRecord(pvd, 156, 20, Sector, true, new byte[] { 0 }); // 34 bytes
        Text(pvd, 190, 128, "");
        Text(pvd, 318, 128, "");
        Text(pvd, 446, 128, "");
        Text(pvd, 574, 128, "SONIC MANIA PS2 DISC BUILDER");
        Text(pvd, 702, 37, ""); Text(pvd, 739, 37, ""); Text(pvd, 776, 37, "");
        Text(pvd, 813, 16, "2026010100000000"); pvd[829] = 0;
        Text(pvd, 830, 16, "2026010100000000"); pvd[846] = 0;
        Text(pvd, 847, 16, "0000000000000000"); pvd[863] = 0;
        Text(pvd, 864, 16, "2026010100000000"); pvd[880] = 0;
        pvd[881] = 1;

        byte[] term = new byte[Sector];
        term[0] = 255; Text(term, 1, 5, "CD001"); term[6] = 1;

        byte[] ptL = new byte[Sector], ptM = new byte[Sector];
        ptL[0] = 1; Le32(ptL, 2, 20); ptL[6] = 1; ptL[7] = 0; ptL[8] = 0;
        ptM[0] = 1; Be32(ptM, 2, 20); ptM[6] = 0; ptM[7] = 1; ptM[8] = 0;

        using (FileStream fs = new FileStream(outPath, FileMode.Create, FileAccess.Write, FileShare.None))
        {
            byte[] zero = new byte[Sector];
            for (int i = 0; i < 16; i++) fs.Write(zero, 0, Sector);
            fs.Write(pvd, 0, Sector);
            fs.Write(term, 0, Sector);
            fs.Write(ptL, 0, Sector);
            fs.Write(ptM, 0, Sector);
            fs.Write(dir, 0, Sector);
            byte[] buf = new byte[1 << 20];
            foreach (Entry e in files)
            {
                long written = 0;
                if (e.Bytes != null) { fs.Write(e.Bytes, 0, e.Bytes.Length); written = e.Bytes.Length; }
                else
                {
                    using (FileStream src = new FileStream(e.Path, FileMode.Open, FileAccess.Read, FileShare.Read))
                    {
                        int n;
                        while ((n = src.Read(buf, 0, buf.Length)) > 0) { fs.Write(buf, 0, n); written += n; }
                    }
                }
                int pad = (int)((Sector - (written % Sector)) % Sector);
                if (pad > 0) fs.Write(zero, 0, pad);
            }
            if (fs.Position != (long)totalSectors * Sector) throw new Exception("internal error: image size mismatch");
        }
    }
}
'@

$entries = New-Object 'System.Collections.Generic.List[Ps2IsoWriter+Entry]'
function Add-File([string]$name, [string]$path) {
    $e = New-Object Ps2IsoWriter+Entry; $e.Name = $name; $e.Path = (Resolve-Path -LiteralPath $path).Path; $entries.Add($e)
}
$cnf = "BOOT2 = cdrom0:\$DiscId;1`r`nVER = 1.00`r`nVMODE = NTSC`r`n"
$e = New-Object Ps2IsoWriter+Entry; $e.Name = 'SYSTEM.CNF'; $e.Bytes = [Text.Encoding]::ASCII.GetBytes($cnf); $entries.Add($e)
Add-File $DiscId $Elf
if ($Data) { Add-File 'DATA.RSDK' $Data }
if ($BootTxt) { Add-File 'BOOT.TXT' $BootTxt }

$outFull = [IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($outFull)) | Out-Null
[Ps2IsoWriter]::Write($outFull, $Label, $entries)
$size = (Get-Item -LiteralPath $outFull).Length
Write-Host ("make_iso: wrote {0} ({1:N0} bytes, {2:N0} sectors), boot file {3}" -f $outFull, $size, ($size / 2048), $DiscId)
exit 0
