<img width="800" height="644" alt="immagine" src="https://github.com/user-attachments/assets/8e32d051-6d31-4d32-a02a-1ea9da23195e" />

# Sonic Mania for PlayStation 2 (unofficial port)

An unofficial, non-commercial port of the community **Sonic Mania Decompilation** to the PlayStation 2. This repository contains **no game data and no SEGA code**.
You build the port yourself, on your own PC, from the game file of **your own legally obtained copy of Sonic Mania Plus**.

> **You need to own Sonic Mania Plus.**  `Data.rsdk` 
> Buy it: [Steam](https://store.steampowered.com/app/584400/Sonic_Mania/)
> 
## Status

Tested on a real PS2 (PAL 50Hz Italian)

## What you need

- Windows 10 or 11.
- [Docker Desktop](https://www.docker.com/products/docker-desktop/), installed and **running**. Nothing else (the PS2 compiler runs inside Docker).
- Internet access for the first build (about 1 GB: the Docker image and the upstream sources). Later builds reuse the downloads.
- About 6 GB of free disk space.
- **Your own `Data.rsdk`** from Sonic Mania Plus. Only the **Steam PC** version has been tested; its SHA-256 is listed in [data-hashes.txt](data-hashes.txt).
  Other versions are untested (the build warns but continues).

## Build

1. Copy `Data.rsdk` from your Sonic Mania Plus folder (next to `SonicMania.exe`) into the **`input`** folder of this repository.
2. Double-click **`build.bat`** (or run it from a terminal). 

`output\ManiaPS2` contains **`SLUS_999.01.SonicManiaPS2.iso`**, a PlayStation 2 disc image (about 215 MB) holding the game and your `Data.rsdk`, and a short `README.txt`.

Options for `build.ps1` (run `powershell -File build.ps1`): `-Refetch` downloads the upstream sources again, `-SkipHashCheck` skips the comparison with the tested versions,
`-KeepElf` also leaves `SonicManiaPS2.elf` and a copy of `Data.rsdk` next to the ISO (for starting it from a USB stick with a homebrew launcher, see below).

## Run it

**On a PlayStation 2:** the console needs a way to run game images, for example Free McBoot or a softmod with Open PS2 Loader (OPL). Put `SLUS_999.01.SonicManiaPS2.iso`
where OPL looks for images (the `CD` folder of a USB stick, a network share or the hard disk; the image is smaller than a CD, so OPL lists it under CD) and start it from OPL.
The disc image contains `SYSTEM.CNF`, the game (`SLUS_999.01`, a plain ELF) and `DATA.RSDK`. The game reads its data from the disc; **Settings.ini, log.txt and your saves are written
to the memory card in slot 1** (folder `SONICMANIAPS2`; the game never formats a card). With no memory card they go to a USB stick if one is plugged in.
`log.txt` is the file to send along with a bug report: on the disc build it is the only record of what the game did. To keep the game fast, `log.txt` is written only at the end of a scene load (at most every 15 s, up to 32 KB) and when the game crashes; it holds the start-up and audio lines, not the per-frame details.

**Report screen:** while playing, hold **R1 + L2 + SELECT** together for half a second. The game stops on a blue text screen showing how long each scene load took, how much of that was file reading and log writing, and the last log lines. Photograph it and restart the console. It also works while a scene is loading (it checks the pad between files), so it can show where a long load is stuck: the screen lists the last file opened. Hold the three buttons for half a second.

**In PCSX2:** File > Start File, choose `SLUS_999.01.SonicManiaPS2.iso` (or `pcsx2-qt -batch -- SLUS_999.01.SonicManiaPS2.iso`). PCSX2 needs a memory card in slot 1.
`tools\pcsx2_smoke.ps1 -Pcsx2 <path to pcsx2-qt.exe>` checks the ISO and boots a test copy of it to the Main Menu.

The game puts a **boot console** on the screen from the first moment: a blue text screen that lists the start-up steps (the version it was built, the console's TV standard, the disc or USB it started from, the memory card, the sound modules, the video mode) while the game loads. It stays until the first game picture appears. You should see it on any TV and cable.

- **TV standard:** the game follows the console's region: a PAL console outputs PAL 576i at 50 Hz (colour on any PAL TV), an NTSC console NTSC 480i at 60 Hz. Game speed stays the same (the logic runs 6 steps for every 5 PAL frames). To force the other one, hold **L1** (NTSC 60 Hz) or **R1** (PAL 50 Hz) while the game starts. NTSC over composite or SCART looks black and white on many PAL TVs.
- **Blue text, then a black screen:** the game's picture is not accepted by your TV or cable. The game starts in **480i** (interlaced, 60 Hz, NTSC), which works on composite, S-Video, SCART and component. It does not use 480p unless you switch it on yourself in Options > Video (that needs component cables and a TV that takes 480p). If you set 480p and lost the picture, **hold SELECT while the game starts**: that forces 480i, and the next change you make in the options saves it.
- **The text stops at some line:** the game is stuck there. Photograph the screen and send it, with `log.txt` from the memory card (folder `SONICMANIAPS2`).
- **Red screen, "CRASH":** the game hit an error. It shows the cause, the address (EPC) and the last start-up lines; photograph it and send it: the address tells where in the program it happened.
- **No text at all, not even blue:** the game did not start (or the console did not run the disc image). Check that the image runs in OPL or PCSX2 and that the console's own region and video settings are correct.
## Settings and saves

- **Options > Video:** Resolution *240P* (a real 240-line picture, 60 Hz NTSC / 50 Hz PAL, no interlace) or *480P* (480 lines, progressive, see Scan mode); Aspect *4:3* (320x240) or *16:9* (424x240); Letterbox; Filter; Scan mode (480i or
  480p, 480 only); **Save Device**. The default is 480i (interlaced, works on every cable), 4:3. Progressive scan needs component cables. Menus lay themselves out for the chosen aspect.
- **Save Device:** *Host* (the folder the game was started from; not offered when the game runs from the disc image, which is read-only, where the memory card is the default), *Memory Card* (slot 1, folder `SONICMANIAPS2`; the game never formats a card), or *USB*.
  Progress is not copied between devices.
- **Data Stream** (Options > Video): *Auto* (default), *On* or *Off*. *On* reads music and assets from the disc image, USB stick or card while you play (the original behaviour). *Off* loads a level's music into memory **before** the level is shown, so nothing is read from the slow medium during play; a **LOADING...** indicator with a rolling Sonic ball appears in the bottom-right corner while a level loads. The level's own music and as many of the standard tracks as fit in memory are loaded; the rest keep streaming. *Auto* turns this on when the game was started from a USB stick, or when a speed test of `Data.rsdk` at start-up measures under 2.5 MB/s (a PS2 reading an image from a USB 1.1 stick manages about 0.5 MB/s), and leaves it off otherwise. The result of the test is in the log (`[boot] media speed`).- **4:3 screens:** to keep the banners readable only the triangle prompt (for example "Game Opts.") is shown, in the bottom-right corner; Back, Confirm and Delete are not drawn.
  The Main Menu labels are drawn smaller so that the whole menu fits.
- Developer options (start-scene, scripted input, tuning) are described in [docs/DEVELOPER_NOTES.md](docs/DEVELOPER_NOTES.md).

## Developer mode (full save)

A hidden mode that unlocks the whole game on your own save file:

1. Main Menu > **Extras** > **Staff Credits**.
2. While the credits roll, press **Up, Up, Down, Down, Left, Right, Left, Right** (d-pad or left stick). A sound confirms the code.
3. Press **Cross + Start** together. The credits end and the **Sound Test** (D.A. Garden) opens, labelled "DEVELOPER MODE".
4. Press **Left** on track 00 (the list wraps to its end) to reach the entry **"DEVELOPER - FULL SAVE"**, then press **Cross**.

It marks every zone cleared, collects all seven emeralds, gives gold medals on all 32 special stages and the good ending, which turns on every unlock (Time Attack, Competition, Peel Out,
Insta-Shield, & Knuckles, Debug Mode, Mean Bean Machine, D.A. Garden, Blue Spheres), and adds a finished Mania Mode save in the first empty slot (saves that hold a game are never touched). The
result is written to the chosen save device. The developer entry stays available until the console is switched off; there is no way to take the unlocks back except deleting the save data.

## How this repository is organised

| Path | What it is |
|---|---|
| `build.bat`, `build.ps1` | The one-step build. |
| `input/` | **Put your `Data.rsdk` here.** Its contents are ignored by git. |
| `output/`, `work/` | Created by the build (ignored by git). |
| `patches/` | Our changes to the two upstream decompilations, as patch files. |
| `src/` | New source files we wrote: the PS2 layer for the RSDKv5 engine (display, audio, pad, memory card and USB, software-rendering helpers). |
| `ps2-build/` | Compile and link scripts for the PS2 Emotion Engine. |
| `docker/` | The pinned build environment and the scripts that run inside it. |
| `docs/` | [Release plan](docs/RELEASE_PLAN.md) (how SEGA's material is kept out), [port notes](docs/PORT_NOTES.md), [developer notes](docs/DEVELOPER_NOTES.md). |
| `licenses/` | The upstream licenses, which also apply to this port. |

At build time the script downloads [Sonic Mania Decompilation](https://github.com/RSDKModding/Sonic-Mania-Decompilation) (pinned commit `9dc69942`) and its dependency
[RSDKv5 Decompilation](https://github.com/Rubberduckycooly/RSDKv5-Decompilation) (pinned `v1.1.1`) from their official repositories, applies `patches/`, adds `src/`, and compiles.

## Licensing

Non-commercial use only. See [LICENSE.md](LICENSE.md), [NOTICE.md](NOTICE.md) and `licenses/`. The decompilations' authors, and the original developers, must be credited; this port's changes are
not theirs and they are not responsible for them.
