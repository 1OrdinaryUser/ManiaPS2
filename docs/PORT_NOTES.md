# Port notes

What the PS2 port does, how it behaves, and what is known to be unfinished. Everything was tested in PCSX2 v2.8.2 and on PAL PlayStation 2.

## How it works

The RSDKv5 engine and the Sonic Mania game logic are compiled for the PS2's Emotion Engine (the `mips64r5900el-ps2-elf` compiler from ps2dev). The engine software-renders each frame into a
16-bit buffer, as it does on every platform; the PS2 layer (`src/`) converts that buffer, uploads it as a texture and draws it as one quad on the Graphics Synthesizer. Audio is mixed
on the EE and queued to the IOP's `audsrv`. The game data is read from `Data.rsdk` next to the ELF; music is streamed from it rather than loaded.

## Memory

32 MB total. About 11 MB is static data; the rest is divided into fixed pools: stage storage 12.8 MB, sound effects 2.7 MB, temporary 2.5 MB, strings 0.25 MB, music 0.6 MB. In the full audit
of 27 acts (PCSX2, 2026-10-17) no pool overflowed and no object was dropped; the sound-effect pool is nearly full in some acts.

## Display

Settings are in Options > Video (stored in `Settings.ini` in the game folder).

| Setting | Behaviour |
|---|---|
| Resolution 240P | The game's own resolution, pixel-perfect (320x240 in 4:3, 424x240 in 16:9), centred in a 640x480 progressive frame with black around it. Filter, Letterbox and Scan mode do not apply. |
| Resolution 480P | 704x480 output. Scan mode chooses 480i or 480p (component video). |
| Aspect | 4:3 renders 320x240, 16:9 renders 424x240. Menus are re-laid out for 4:3: the Main Menu staircase is drawn at 65% size with its labels area-averaged (not nearest-neighbour) so small text stays readable, and the Back / Confirm / Delete prompts are not drawn, only the triangle prompt in the bottom-right corner, because they overlapped the menu banners. 16:9 is unchanged. |
| Letterbox | Only with 16:9: true shape inside a 4:3 frame (bars above and below) or stretched to fill. |
| Filter | Clean (nearest neighbour) or Filtered (bilinear). |

Defaults: 480p progressive, 4:3, clean. NTSC only. The 480i modes show 448 of the 480 lines, as a real NTSC set does; the picture is fitted into them.

## Developer mode

Extras > Staff Credits, then Up Up Down Down Left Right Left Right and Cross + Start opens the Sound Test with an extra entry, "DEVELOPER - FULL SAVE" (Left on track 00), which writes a 100% save and
unlocks everything on the player's own save. See README.md. It is implemented in `CreditsSetup.c` and `DASetup.c` (part of `patches/sonic-mania-decompilation.patch`); the flag is bit 8 of the game's `secrets` variable.

## Saves

*Save Device* in Options > Video: **Host** (the folder the game started from; the default), **Memory Card** (slot 1, folder `SONICMANIAPS2`; a card that is not formatted is never formatted by the game),
or **USB** (`mass:/SONICMANIAPS2`). Progress is not copied between devices. Writes are synchronous and pause the sound briefly.

## Booting from USB

The USB drivers are linked into the ELF, so no other file is needed. When the ELF is started from `mass:/folder/`, the game reads `Data.rsdk` and writes `Settings.ini` and `log.txt` there.
Opening a very large `Data.rsdk` on a FAT32 volume with small clusters is slow (each open or seek is proportional to the file's cluster count): use 32 KB clusters.

## Performance (PCSX2, built-in test bot)

Share of frames that took one vblank (60 FPS on time). Real-hardware speed is unknown.

| Scene | On time |
|---|---|
| Green Hill 1, Metallic Madness 1, Flying Battery 1 | 100% |
| Chemical Plant 2 | about 81-83% |
| Press Garden 1 | about 57% |
| Titanic Monarch 3 | about 67-68% |
| Special stages 1-7 | about 71-92% |
| Blue Spheres, Pinball | about 89-90% |

The main cost in the slow acts is decoding the music in software (about 20% of the CPU) on top of full-screen software tile drawing. To hold the frame rate the game lowers detail when frames run
long: far scenery in special stages is not drawn, the special-stage floor and parallax backgrounds draw every second row. This only happens while the frame rate is below target.
