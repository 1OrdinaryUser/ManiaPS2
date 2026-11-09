# Release plan: shipping the PS2 port without shipping SEGA's material

This document records how the repository is split so that it contains only (a) our own work and (b) instructions that fetch everything else from its rightful
source at build time, and (c) a slot for the user's own copy of the game data. It is a design note, not legal advice: have someone qualified review it before
a public release.

## 1. What is protected, and who owns it

| Material | Owner | In this repository? |
|---|---|---|
| `Data.rsdk` (sprites, music, sound, levels, text, meshes: all game assets) | SEGA / Headcannon | **No.** The user supplies their own copy (`input/Data.rsdk`). |
| The Sonic Mania game logic, even as decompiled C | SEGA / Headcannon | **No.** Fetched from the upstream decompilation repository at a pinned commit. |
| The RSDKv5 engine, even as decompiled C++ | Evening Star (Taxman) / SEGA | **No.** Fetched from the upstream decompilation repository at a pinned tag. |
| A built `.elf` or disc image (contains both of the above, compiled, plus assets if packaged) | SEGA / Headcannon / Evening Star | **No.** Built on the user's machine, for their own use. Never committed, never attached to a release. |
| Screenshots, videos, logs and audit files taken from the running game | SEGA / Headcannon | **No.** Excluded (the working tree had 444 PNG screenshots and 83 MB of such files). |
| User saves, `Settings.ini`, `SaveData.bin`, `Achievements.bin` | The user | **No.** |
| Our changes to the two decompilations | Us, as modifications of the above | **Yes, as patch files only** (`patches/`). A patch contains our changed lines and a few lines of upstream context; no complete upstream file. |
| New source files we wrote (PS2 hardware layer, PS2 render/audio/input backends, software rasteriser helpers) | Us (written as backends of the RSDKv5 engine) | **Yes** (`src/`). |
| Build tooling (scripts, Dockerfile), documentation | Us | **Yes.** |
| PS2 SDK, gsKit, audsrv, IOP modules, compiler | ps2dev project (AFL 3.0 / GPL / LGPL, per component) | **No.** Pulled by Docker from the pinned `ps2dev/ps2dev` image at build time. |
| tinyxml2, stb_vorbis, miniz, iniparser | their authors (zlib / MIT / public domain) | **No.** They come with the upstream clone (submodules / bundled dependencies). |

## 2. Rules we follow from the upstream licenses

Both upstream projects are distributed under their own source licenses (copies in `licenses/`). The conditions that shape this release:

1. Non-commercial use only. The release says so, and nothing in it may be sold or bundled with anything sold.
2. The original developers must be credited, and it must be clear that the code is not ours (`NOTICE.md`, `README.md`).
3. No game assets may be distributed that are not provided by the upstream repository. Hence the user supplies `Data.rsdk`.
4. The license must be present in all modified copies of the source. It is in `licenses/`, and the patches carry the license names in the README.
5. RSDKv5 license v2.1 only: **pre-built executables given to anyone must be built with DLC disabled by default.** We do not distribute any executable. The user
   builds their own, which is a private build; the README says the result must not be redistributed. (Our build enables the Plus content because the user's own
   Plus data file is what it runs.)

## 3. How the build keeps them apart

```
User's PC                                          Docker container (pinned ps2dev image)
---------                                          --------------------------------------
input/Data.rsdk  (user supplies)  ------------------------------------------------+
release repo: patches/, src/, ps2-build/ ----> work/cache   : git clone of upstream at pinned commits (first run only)
                                               work/build   : copy of the clone + our patches + our new files
                                                              compile for the PS2 EE, link  -> p2_engine.elf
output/ManiaPS2/  <-----------------------------------------  SonicManiaPS2.elf  + Data.rsdk copied next to it
```

- `build.bat` / `build.ps1` is the single entry point. It checks Docker, checks that `input/Data.rsdk` exists and really is an RSDKv5 data file, builds the container,
  fetches upstream, applies the patches, builds, and assembles `output/ManiaPS2/`.
- The upstream clones are pinned by commit (Sonic Mania Decompilation `9dc69942`, which pins RSDKv5-Decompilation `v1.1.1` and its submodules). A different upstream
  state is never silently used.
- The `Data.rsdk` file is only ever copied from `input/` to `output/ManiaPS2/` (next to the ELF, which is where the game looks for it). It is not read by the build.
- `.gitignore` excludes `input/*`, `work/`, `output/`, and every build artifact type, so a careless `git add .` cannot add them. A check script (`tools/check_release_tree.ps1`)
  scans the tree for `.rsdk`, `.elf`, `.iso`, images, and binaries and fails if it finds any.

## 4. What the user must do

1. Own a legal copy of Sonic Mania Plus (the tested file is the Steam PC release, version 1.06).
2. Install Docker Desktop (Windows). No other tools are needed.
3. Copy `Data.rsdk` from their game folder into `input/`.
4. Run `build.bat`. The first run downloads about 1 GB (Docker image and upstream sources) and takes several minutes.
5. Copy `output/ManiaPS2/` to a FAT32 USB drive (or point PCSX2 at the ELF) and start the ELF with a homebrew launcher.

## 5. Checks done before release

- Clean-room build: a copy of the release folder in an empty directory, only `Data.rsdk` added, `build.bat` run, ELF produced.
- The produced ELF boots to the Main Menu in PCSX2 (`tools/pcsx2_smoke.ps1`).
- The release tree contains no `.rsdk`, `.elf`, `.iso`, image, or save file (`tools/check_release_tree.ps1`).
- The patches apply to a pristine upstream clone (done by the build itself).

## 6. Open items for the maintainer

- Confirm the license wording in `LICENSE.md` (original files under the same non-commercial terms as upstream; build tooling and docs under MIT).
- Have the legal position of publishing patches against the SEGA-derived decompilations reviewed. The upstream license explicitly allows distributing modifications under its conditions,
  but it is not a license from SEGA.
- Real-hardware behaviour is untested. The README says so.
- A Linux/macOS build script is not provided or tested (the container steps are portable; only `build.ps1` is Windows-specific).
- The developer mode (hidden full-save option, see README) only edits the player's own save file; it adds no content and no data from the game.

## 7. Verification record

First release (2026-10-18), done on the finished release folder copied to an empty directory with only a `Data.rsdk` added:

- The build script fetched Sonic Mania Decompilation `9dc69942` and RSDKv5 `v1.1.1` (with tinyxml2 `e45d9d1` and stb_vorbis `af1a5bc`), both commit checks passed, both patches applied, all 32 compile units
  and the link succeeded, and it produced `output\ManiaPS2\SonicManiaPS2.elf` (7,381,824 bytes). A second run with the cached clone took about 2 minutes.
- `tools\pcsx2_smoke.ps1` booted that ELF in PCSX2 v2.8.2 to the Main Menu (PASS), and Chemical Plant Zone 1 rendered correctly when started directly.
- The preflight refused a missing file, a file with the wrong header and a truncated file, each with a clear message and exit code 1; it recognised the tested Steam `Data.rsdk` by its SHA-256.
- `tools\check_release_tree.ps1` passes on the release folder and fails on planted decoys (a `.elf`, a settings file, a renamed data-pack header).

Second release (2026-10-19, developer mode, 4:3 prompts and Main Menu text), same procedure with freshly regenerated patches (Sonic Mania patch 25 files, RSDKv5 patch 27 files, no new-file entries):

- Clean build from an empty directory succeeded (32 units, `Game=0 All=0 LINK=0`), ELF 7,387,752 bytes.
- `tools\pcsx2_smoke.ps1` PASS on the release-built ELF.
- The developer-mode path (Extras > Staff Credits > code > Cross + Start > Sound Test > Left > Cross) was run on the release-built ELF with scripted input: the Sound Test showed "DEVELOPER MODE" and "FULL SAVE WRITTEN",
  with 0 TLB misses and 0 storage-header errors. On the development build the written save was reloaded: the first empty slot showed CLEAR with seven emeralds, and Blue Spheres, Mean Bean Machine and D.A. Garden were unlocked.
- Not verified: a machine without the existing Docker image cache or toolchain download, PowerShell 7, Linux or macOS, and any real PlayStation 2.

Third release (2026-10-19, 4:3 Main Menu speed and input), `patches/rsdkv5.patch` regenerated (still 27 files, `Drawing.cpp` and `RetroEngine.cpp` changed) and `src/.../PS2/ps2_hal.h` updated:

- Cause of the 30 FPS Main Menu in 4:3: the area-averaged label drawing cost about 10.7 ms per frame, so the frame took two vblanks. The reduced labels are now built once and kept in a 16-entry cache
  (flushed when a scene loads). In PCSX2 the 4:3 Main Menu went from 0/300 to 290-295/300 frames on time (`UIButton` 10.7 ms to 0.58 ms per frame); 16:9 is unchanged.
- Cause of the skipped entry: when a frame is slow the engine runs extra 60 Hz logic steps, and every step saw the same button press. `ClearInput()` now runs before each extra step. Checked with two logic steps
  forced: one Down moved to Encore Mode with the fix and to Time Attack without it.
- Build from the release folder (patches applied, `Game=0 All=0`), ELF 7,394,436 bytes, SHA-256 `09C9000E35CA58A25D2C49C0D376BF45990CA1969B85CCBA5F0B721A7972CC6E`. `tools\pcsx2_smoke.ps1` PASS; the release ELF showed
  0 TLB misses, 0 storage-header errors. Not verified on a real PlayStation 2.

Fourth release (2026-10-22, disc image output), `patches/rsdkv5.patch` regenerated (28 files), `src/.../PS2/ps2_hal.c` and `ps2_hal.h` updated, new `tools/make_iso.ps1`:

- The build now ends with a PlayStation 2 disc image, `output\ManiaPS2\SLUS_999.01.SonicManiaPS2.iso` (ISO 9660 with `SYSTEM.CNF`, the game as `SLUS_999.01` and `DATA.RSDK`), written by a small ISO writer in `make_iso.ps1` (no extra tool needed; fixed dates, so the same input gives the same file). `-KeepElf` also leaves the ELF and `Data.rsdk`.
- The game recognises a disc start from `argv[0]` (`cdrom0:\...`), reads `cdrom0:\DATA.RSDK;1`, and keeps Settings.ini, log.txt and saves on the memory card (else USB). Disc boot never uses the `host` save device.
- `tools/pcsx2_smoke.ps1` mounts the real ISO read-only in Windows to list it, then boots a test copy (same ELF and Data.rsdk plus a `BOOT.TXT`) in PCSX2 as a disc image: PASS (Data.rsdk loaded from `cdrom0`, Main Menu reached, 0 TLB misses). Save self-tests (`savetest`, `usersavetest`) pass from a disc boot. The plain ELF path was re-checked in PCSX2 and behaves as before.
- Learned: the memory card file driver ignores the append flag (each write started at offset 0), so the log is appended by opening with `r+b` and seeking to the end.
- Not verified: a real PlayStation 2, OPL, a burned disc, and loading speed from a real drive (in PCSX2 the Main Menu is reached after about 45-55 s from the image, 40 s from the host folder).

Fifth release (2026-10-27, boot console, crash screen, 480i default), `patches/rsdkv5.patch` regenerated (28 files), `ps2-build/Makefile.link` now links `-ldebug`:

- Reason: the disc/ELF showed a black screen on a real console. Root cause candidate found in the code: the game started in DTV 480p progressive (and the `240p` mode also uses it), which only shows on component cables. The defaults are now 480i; SELECT at start forces 480i; a blue boot console lists the start-up steps; a red crash screen shows exceptions.
- Verified in PCSX2: boot console contents, crash screen during start-up and during the game, normal boot to the Main Menu in 480i, disc smoke test. Not verified: any real console. Whether 480p was the actual cause is not known.
- Heap: the first version of the crash stacks used 24 KB of static arrays, which moved the heap limit by 64 KB and made the Menu scene load very slowly (the Main Menu was not reached in 240 s). They are now in the scratchpad.

Sixth release (2026-10-27, PAL output, early console), `patches/rsdkv5.patch` regenerated (28 files):

- The user's console test (PAL console, SCART, OPL from USB) showed: text screen fine, PAL 576i bars fine, NTSC 480i bars without colour, 480p black. The game therefore now outputs PAL 576i / 50 Hz on a PAL console (`gsKit_detect_signal`), NTSC 480i otherwise; L1 / R1 at start force NTSC / PAL, boot.txt `std=ntsc|pal`. Logic keeps 60 steps a second (6 per 5 PAL fields). The disc image is named `SLUS_999.01.SonicManiaPS2.iso`.
- A static initialiser (priority 150) starts the boot console and crash handlers before the engine's own initialisers; `main() reached` is logged.
- Verified in PCSX2: PAL (this BIOS reports PAL) Main Menu at 300/300 frames on time and a correct picture; `std=ntsc` path unchanged; disc smoke test. Not verified: the game on a real console (it was still black before this change, with no console either).

Seventh release (2026-10-27, audio on slow frames, 1.5x boot console), `patches/rsdkv5.patch` regenerated (28 files):

- First build that runs on the user's console (PAL, SCART, OPL from USB). Audio was only heard in the Main Menu. `ps2_midframe()` call sites in `Object.cpp` and `RetroEngine.cpp` plus `AudioDevice::PumpIfLate` keep the sound buffer fed during long frames (see STATUS_AND_PLAN section 21). Verified only with the simulated slow console (`burn=`) in PCSX2, not on the console.
- Boot console 1.5x, crash screen 2x. Smoke test PASS.