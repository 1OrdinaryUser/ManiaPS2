# Developer notes

## boot.txt

A text file named `boot.txt` next to the ELF is read at start (one line, options separated by `;` or spaces). It is for testing and is never needed to play. On a disc image the same file is `BOOT.TXT` in the root: `tools\make_iso.ps1 ... -BootTxt boot.txt` puts it there (the normal build adds none)

| Option | Effect |
|---|---|
| `stage=GHZ;scene=1;filter=3` | Start directly in that scene (filter 3 = Mania Mode, 255 = special and bonus stages). |
| `bot` | After the title, hold RIGHT and jump in a pattern (unattended play). |
| `inject=FRAME:BUTTON:FRAMES/...` | Scripted pad presses on port 0, e.g. `inject=1400:START:6/2000:CROSS:6`. Buttons: START SELECT UP DOWN LEFT RIGHT CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2. Frames count pad polls, so they do not advance during scene loads. |
| `menu=Save_Select` | Open a menu by its tag (underscores for spaces). `openvideo` opens Options > Video. |
| `vid=<240\|480>:<i\|p>:<43\|169>:<letterbox 0\|1>:<c\|f>` | Pin the display mode; `vidswap=<frame>:<same fields>` switches mode mid-run. |
| `save=host\|mc\|usb` | Pin the save device. `bootdir=mass:/` tests the USB path under PCSX2. `mcformat`, `savetest`, `usersavetest`, `usbdiag` are device tests. |
| `noaudio`, `nopause`, `eagerstream`, `audiocheck` | Audio A/B switches. |
| `lod=0..3` | Pin the 3D detail level (without it the level adapts between 0 and 6 to the frame time; `lodcmp=N` compares a frame at level 0 and N). |
| `tune0=N` .. `tune7=N` | Override scheduler constants without a rebuild: 0-3 audio mixing (ring-free threshold 8192, gate percent 150, extra us 300, forced slices per frame 4); 4-5 3D detail thresholds (12000, 9500 us); 6 = 1 turns background half-rows off; 7 = detail level that switches them on (4). |
| `achtest`, `strstress`, `mmitest` | Self-tests (achievement banner, string pool, MMI blend kernel). |

Example, the whole developer-mode path in PCSX2 (open Extras, move to Staff Credits, enter the code, Cross + Start, Left, Cross):
`menu=Extras inject=2000:DOWN:4/2060:DOWN:4/2120:DOWN:4/2200:CROSS:6/3000:UP:4/3040:UP:4/3080:DOWN:4/3120:DOWN:4/3160:LEFT:4/3200:RIGHT:4/3240:LEFT:4/3280:RIGHT:4/3400:CROSS:14/3400:START:14/4300:LEFT:4/4500:CROSS:6`

## Logs

`log.txt` next to the ELF (on a disc boot: in `SONICMANIAPS2` on the memory card, written about once a second for the first minute and then in larger blocks) is the engine's own log (scene loads, files). The EE console (PCSX2: enable *EE Console* logging) carries the port's diagnostics. Every 300 frames:

```
[prof] avg/frame over 300: engine=.. convert=.. queue=.. vsync-wait=.. total=..
[prof]   pacing: N frames on time (1 vblank), N took 2, N took 3+, worst frame .. us
[prof]   work time per frame (ms, CPU only): <8:.. 8-10:.. ... 22+:..
[prof]   engine split: logic=.. draw=.. (tile-layers=.. entities=..) other=.. us
[prof]     class <name>  <us>/frame  (<n> draws/frame)
```

and a `[slow]` line for each frame that took more than 22 ms: its logic, draw, tile-layer, entity, convert, queue and **audio** time and the two most expensive object classes.
If `work` is over 16.7 ms the frame did too much; the biggest term says what to cut. Audio mixing runs inside the present call (the vsync wait), so it only shows in `[slow]`.
`[audio]` lines (every 600 frames) report mix cost per slice and the ring-buffer health.

## A pitfall: object static data and Game.c

`RSDK_REGISTER_OBJECT` in `Game.c` records `sizeof(Object<Name>)`. Adding a field to an object's static struct (its `.h`) without recompiling `Game.c` makes the engine allocate the old, smaller block, and the new
fields overwrite the next storage block's header; the next scene change then crashes in `DefragmentAndGarbageCollectStorage` with a wild-pointer loop (hundreds of `TLB Miss` lines). `build.ps1` always builds
everything, so a release build is not affected; when rebuilding by hand, rebuild `Game.c` too. The engine logs `[HDRBAD] pool ...` just before the scene change when a pool's headers are inconsistent.

## Rebuilding after a change

`build.ps1` always does a clean build from the downloaded upstream sources plus `patches/` and `src/`. To change the port, edit the files in `work\build\files\...` for quick experiments, then regenerate
the patches (`git diff` against the pinned upstream commits) and copy any new files into `src/`.

## Things measured and rejected

Scanline or palette caching in the tile layers (the cost is the per-pixel blit, not the setup), row classification and prefetching in the tile blit (no change in PCSX2, which does not model the EE data cache),
and audio scheduling variants (the average audio cost plus the engine cost exceeds a frame in the heaviest acts, so the schedule cannot fix it).

## Boot console and crash screen (real-console debugging)

A console has no serial log. `ps2_diag_init()` (first thing in `main`) puts up a libdebug text console that mirrors the `[boot]`, `[iop]`, `[audio]`, `[save]`, `[usb]`, `[gfx]`, `[settings]`, `[diag]`, `[crash]`, `[mem] render`, `Loaded file`, `File not found`, `Loading Scene` and `Main Menu` log lines. The game's own video mode is not started until the first game frame (`ps2_gfx_present`), so the text stays up for the whole load; at that moment a `[diag] starting game video` line shows which mode is about to be used, the screen waits 1.8 s, and the game video starts.

- Exceptions: `SetVCommonHandler` / `SetVTLBRefillHandler` send every exception except syscalls to `ps2_exc_entry`. Drawing from inside the exception fails (init_scr uses BIOS calls), so the entry code moves to a scratchpad stack, loads the program's `_gp`, logs the registers, clears `Status.EXL` and jumps to `ps2_crash_screen`, which runs as ordinary code, redraws the text screen in red with cause, EPC, BadVAddr and the last lines, flushes the log, and stops. Returning to the kernel with a changed EPC does not work (the kernel restores its own copy).
- The two stacks are in the scratchpad (0x70000000) because the heap is full to the last few KB: a first version with 24 KB of ordinary arrays shifted the heap limit by 64 KB (20352 to 20288 KB) and the Menu scene then loaded about ten times slower. Check `[boot] ... max heap growth=20352 KB` after any change to static data.
- Look up a crash address with the build's map: `work\build\prototypes\p2_engine\p2_engine.map` (or `addr2line` in the container on `p2_engine.elf`).
- boot.txt: `crashtest=boot` (breakpoint exception right after the console is up) and `crashtest=game` (some frames into the game) test the crash screen. PCSX2 does not raise address errors, so the test uses `break`.
- `ps2_safe_video()`: SELECT held at start forces 480 lines interlaced (video settings read from Settings.ini are overridden for that run).
