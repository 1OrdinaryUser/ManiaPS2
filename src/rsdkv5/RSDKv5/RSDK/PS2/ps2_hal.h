/*
 * PS2 hardware abstraction layer for the RSDK port.
 *
 * Plain C on purpose: gsKit/ps2sdk headers define macros and types that clash with the
 * C++ engine, so the engine only ever sees this small interface.
 */
#ifndef PS2_HAL_H
#define PS2_HAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Graphics ---- */

/* Initialise the GS (NTSC 640x448, double buffered) and a texture able to hold texW x texH RGB565. */
int ps2_gfx_init(int texW, int texH);

/* Convert and present an RGB565 frame (width x height, row pitch in pixels) as one stretched quad.
 * Blocks until vertical sync. */
void ps2_gfx_present(const uint16_t *rgb565, int pitchPixels, int width, int height);

void ps2_gfx_shutdown(void);

/* Video options (applied on the next present):
 *   hires       = 480 lines (else 240p, which is always non-interlaced)
 *   progressive = 480p instead of 480i (only matters when hires)
 *   widescreen  = 16:9 picture, 424x240 (else 4:3 picture, 320x240). Query ps2_gfx_wide() to size the render buffer.
 *   letterbox   = with a 16:9 picture: show it at its true shape with black bars above and below (for 4:3 TVs); off = fill the frame
 *                 (anamorphic: a widescreen TV stretches it back to 16:9). No effect on a 4:3 picture.
 *   filtered    = bilinear filtering when scaling to the output (else nearest neighbour, "clean"). */
/* Called once per presented frame, after the GS work is queued and before waiting for vertical sync (the EE would otherwise idle). */
void ps2_gfx_set_idle_hook(void (*fn)(void));
/* Hook run from ps2_midframe(), which the engine calls between draw groups, tile layers and logic steps (see PS2AudioDevice.cpp: PumpIfLate). */
void ps2_gfx_set_mid_hook(void (*fn)(void));
void ps2_midframe(void);
/* Estimated microseconds until the next vertical blank (for deciding whether idle work still fits in this frame). */
uint32_t ps2_vblank_remaining_us(void);
void ps2_gfx_configure(int hires, int progressive, int widescreen, int letterbox, int filtered);
/* ---- Save media ----
 * Where the game's user files (SaveData.bin, Achievements.bin, ...) live.
 *   PS2_SAVE_HOST  the host: filesystem next to the ELF (PCSX2 development; the original behaviour)
 *   PS2_SAVE_MC    memory card slot 1, folder SONICMANIAPS2
 *   PS2_SAVE_USB   USB mass storage, folder mass:/SONICMANIAPS2 (IOP modules are embedded in the ELF and started on first use)
 * The ps2_save_* calls are used for every device except HOST (which keeps using plain stdio). */
#define PS2_SAVE_HOST 0
#define PS2_SAVE_MC   1
#define PS2_SAVE_USB  2
void ps2_save_set_device(int device);
void ps2_save_pin_device(int device);             /* boot.txt save=...: wins over Settings.ini */
void ps2_save_set_device_by_name(const char *name); /* Settings.ini saveDevice=host|mc|usb */
const char *ps2_save_device_name(void);           /* "host", "mc" or "usb" (what Settings.ini stores) */
int ps2_save_device_available(int device);        /* 1 if saves can be stored on that device now (USB: only once its modules loaded) */
void ps2_save_peek_settings(const char *path);    /* early scan of Settings.ini for saveDevice= (before the engine loads anything) */
int ps2_save_device(void);

/* Where the game was started from. Data.rsdk, Settings.ini, log.txt and (for the HOST save device) the saves all live here.
 *   "host:"            PCSX2 development (ELF directory)
 *   "mass:/" or "mass:/dir/"   booted from a USB stick: the USB driver is started first, then the same files are read from the stick
 * ps2_boot_dir_from_elf() derives it from argv[0] (empty or unknown -> "host:"); ps2_boot_dir_set() is the boot.txt bootdir= test override. */
void ps2_boot_dir_from_elf(const char *elfPath);
void ps2_boot_dir_set(const char *dir);
const char *ps2_boot_dir(void);
int ps2_boot_dir_is_usb(void);

/* ---- Data streaming (Settings > Video > DATA STREAM) and the loading indicator ----
 * ps2_stream_mode: 0 = AUTO, 1 = ON (read while playing), 2 = OFF (preload before the level, indicator shown). ps2_preload_active() is the effective answer
 * (AUTO measures the read speed of Data.rsdk once, and counts a USB start as slow). The loading hook draws the indicator (PS2RenderDevice.cpp). */
int ps2_stream_mode(void);
void ps2_set_stream_mode(int m);
void ps2_pin_stream_mode(int m); /* boot.txt stream=on|off: wins over Settings.ini */
void ps2_set_stream_mode_by_name(const char *name); /* Settings.ini dataStream=auto|on|off */
const char *ps2_stream_mode_name(void);
int ps2_preload_active(void);
void ps2_set_usbsim(int ms);  /* developer: boot.txt usbsim=MS */
void ps2_io_log(const char *what);
void ps2_note_file(const char *name); /* LoadFile: the file being opened, for the report screen */
void ps2_report_poll_pad(void);
void ps2_set_reportat(int s); /* developer: boot.txt reportat=S */
void ps2_set_bonustest(int on); /* developer: boot.txt bonustest */
int ps2_bonus_test_due(void);
void ps2_load_begin_mark(void); /* report screen: a scene load begins / ends (name = scene folder) */
void ps2_load_end_mark(const char *name);
void ps2_report_screen(void);
void ps2_s3d_release(void); /* Scene3D.cpp: frees the special stage scratch buffers (call when a scene load begins) */
void ps2_log_checkpoint(void); /* disc / USB boot: writes the buffered log.txt lines if the last write is 15 s old (call where a delay is hidden) */
void ps2_set_readdelay(int ms); /* developer: boot.txt readdelay=MS */
void ps2_set_loading_hook(void (*fn)(int phase));
void ps2_loading_begin(void);
void ps2_loading_tick(void);
void ps2_loading_end(void);
int ps2_loading_active(void);
size_t ps2_read_ticked(void *dst, size_t n, void *file);
/* music preload (Audio.cpp, C++ with C linkage): the game registers its track files; the engine reads them into memory before the level is shown */
void ps2_stream_register(const char *file, int prio);
void ps2_stream_clear(void);
void ps2_stream_preload_all(void);
/* ---- Boot console and crash screen ----
 * A console shows a black screen when a start fails. ps2_diag_init() (first thing in main) puts up a text console (libdebug) that lists the start-up
 * steps (the [boot] / [iop] / [audio] / [save] / [gfx] ... log lines) while the game loads; the first game frame replaces it. An exception at any
 * time shows a crash screen (cause, program counter, last lines). ps2_diag() adds a line. ps2_safe_video() is 1 when SELECT was held while starting:
 * the game then uses 480 lines interlaced whatever its Settings.ini says (a way back from a video mode the screen cannot show). */
void ps2_diag_init(void);
void ps2_diag(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int ps2_safe_video(void);
void ps2_stage(const char *text); /* notes the start-up step that begins now; the crash screen shows the last one (the text must be a string literal) */
void ps2_set_burn_ms(int ms); /* developer: boot.txt burn=MS wastes MS milliseconds per frame (imitates a slower console) */
void ps2_set_tv_std(int std); /* 0 = the console's own TV standard (default), 1 = NTSC, 2 = PAL; hold L1 / R1 at start, boot.txt std=ntsc|pal. PAL output is 576i at 50 Hz */
void ps2_set_crashtest(int mode); /* developer: 1 = fault right after the console is up, 2 = fault some frames into the game */
/* ---- Disc boot ----
 * Started from a disc image (argv[0] = "cdrom0:\SLUS_999.01;1"; the ISO the release builder makes, run from OPL or a burned disc), the game's
 * files are on a read-only ISO 9660 volume: Data.rsdk (and a developer boot.txt) are read with ps2_disc_path() names ("cdrom0:\DATA.RSDK;1"),
 * and everything the game writes (Settings.ini, log.txt, saves) goes to the memory card, or to a USB stick if there is no card.
 *   ps2_boot_dir_is_disc()          1 when started from a disc
 *   ps2_disc_path(out, size, name)  "Data.rsdk" -> "cdrom0:\DATA.RSDK;1" (upper case, 8.3 style names, version suffix)
 *   ps2_disc_user_dir_prepare()     picks the device for the user files (memory card, else USB, else none) and starts it; call once, early
 *   ps2_user_dir()                  prefix for Settings.ini / log.txt / host-style files: "mc0:/SONICMANIAPS2/", the USB folder, the boot directory
 *                                   of a USB boot, or "" (PCSX2: relative paths)
 *   ps2_user_log_write(text)        disc boot only: appends to log.txt on that device, buffered (about a second at most); returns 0 when not a disc boot */
int ps2_boot_dir_is_disc(void);
void ps2_disc_path(char *out, unsigned size, const char *name);
void ps2_disc_user_dir_prepare(void);
const char *ps2_user_dir(void);
int ps2_user_log_write(const char *text);
int ps2_save_read(const char *name, void *buffer, unsigned size);        /* bytes read (<= size), or -1 if missing / not readable */
int ps2_save_write(const char *name, const void *buffer, unsigned size); /* 0 on success, -1 on failure */
int ps2_save_delete(const char *name);                                   /* 0 on success */
void ps2_save_allow_format(int allow);                                   /* developer only (boot.txt mcformat): format an unformatted card */
void ps2_save_selftest(void);                                            /* writes, reads back and verifies a 64KB file; logs [save] PASS/FAIL */

/* Developer testing: pin the display settings (boot.txt vid=...), ignoring later ps2_gfx_configure calls. */
void ps2_gfx_set_override(int hires, int progressive, int widescreen, int letterbox, int filtered);
/* 1 = render a 16:9 picture (424 wide), 0 = 4:3 (320 wide). Valid after ps2_gfx_configure. */
int ps2_gfx_wide(void);
/* Developer testing: after atFrame presented frames, pin a different display mode (boot.txt vidswap=...). */
void ps2_gfx_schedule_swap(uint32_t atFrame, int hires, int progressive, int widescreen, int letterbox, int filtered);

/* printf-style output to the EE SIO console (shown by PCSX2 when EE Console logging is enabled). */
void ps2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Logs malloc arena/used/free and the program break. */
void ps2_heap_report(const char *label);

/* Breadcrumb for bring-up: PS2_TRACE("opening %s", name); */
#define PS2_TRACE(...)                                                                                                                               \
    do {                                                                                                                                             \
        ps2_log("[trace] " __VA_ARGS__);                                                                                                             \
        ps2_log("\n");                                                                                                                               \
    } while (0)

/* Early platform setup (RPC init, console banner). Call first thing in main(). */
void ps2_boot(void);

/* ---- Timing ---- */

/* Monotonic microseconds. */
uint64_t ps2_time_us(void);

/* EE cycle counter (294.912 MHz, wraps every ~14.5 s): subtract two readings for an interval. */
uint32_t ps2_cycles(void);

/* Frame profiler: accumulate EE cycles into a slot; the totals are logged every 300 presented frames. */
#define PS2_PROF_LOGIC 0 /* ProcessObjects: game logic */
#define PS2_PROF_DRAW  1 /* ProcessObjectDrawLists: software rasteriser */
/* Number of 60 Hz logic steps to run for the next frame (1..3), from how many vblanks the last frame took. */
uint32_t ps2_logic_steps(void);
/* Empties the cache of reduced sprites kept by the Main Menu text drawing (Drawing.cpp); called when a scene loads. */
void ps2_flush_smooth_cache(void);

#define PS2_PROF_LAYERS   2 /* tile layers (DrawLayer*, parallax) inside the draw lists */
#define PS2_PROF_ENTITIES 3 /* entity Draw() callbacks inside the draw lists */
#define PS2_PROF_SLOTS 4
void ps2_prof_add(int slot, uint32_t cycles);

/* Attribute entity draw time to an object class name (pointer identity; names are static strings). The top few are logged. */
void ps2_prof_class(const char *name, uint32_t cycles);

/* Draw audit: called after every entity Draw(). centred = entity position is inside the screen, drew = the draw call put pixels down.
 * Cumulative per class; classes that are often on screen yet never draw are listed periodically (a lead for missing sprites). */
void ps2_audit_draw(const char *name, int centred, int drew);

/* Print the audit now (problem classes + summary). */
void ps2_audit_dump(void);

/* Number of recorded on-screen Draw() calls for a class (0 if never seen). Names compare by pointer, like ps2_audit_draw(). */
unsigned ps2_audit_seen(const char *name);

/* Gate recording (used by the camera tour so only settled frames count) and clear all tallies. */
void ps2_audit_enable(int enabled);
void ps2_audit_reset(void);

#ifdef __cplusplus
/* Time a statement in EE cycles and add it to a profiler slot. Engine files define it as a no-op on other platforms. */
#define PS2_PROF(slot, stmt)                                                                                                                         \
    do {                                                                                                                                             \
        uint32_t ps2ProfT0 = ps2_cycles();                                                                                                           \
        stmt;                                                                                                                                        \
        ps2_prof_add(slot, ps2_cycles() - ps2ProfT0);                                                                                                \
    } while (0)
#endif

/* ---- Pad ---- */

#define PS2_PAD_SELECT   0x0001
#define PS2_PAD_L3       0x0002
#define PS2_PAD_R3       0x0004
#define PS2_PAD_START    0x0008
#define PS2_PAD_UP       0x0010
#define PS2_PAD_RIGHT    0x0020
#define PS2_PAD_DOWN     0x0040
#define PS2_PAD_LEFT     0x0080
#define PS2_PAD_L2       0x0100
#define PS2_PAD_R2       0x0200
#define PS2_PAD_L1       0x0400
#define PS2_PAD_R1       0x0800
#define PS2_PAD_TRIANGLE 0x1000
#define PS2_PAD_CIRCLE   0x2000
#define PS2_PAD_CROSS    0x4000
#define PS2_PAD_SQUARE   0x8000

typedef struct ps2_pad_state {
    uint16_t buttons; /* PS2_PAD_* bits, active high */
    uint8_t lx, ly;   /* left stick, 0..255, centre ~128 */
    uint8_t rx, ry;   /* right stick */
    uint8_t connected;
} ps2_pad_state;

int ps2_pad_init(void);
/* Developer testing: scripted pad presses, see ps2_hal.c (boot.txt inject=...). */
void ps2_inject_set(const char *spec);
/* Developer testing: boot.txt "noaudio" skips the audio mixer entirely, to measure how much frame time audio costs. */
void ps2_set_noaudio(int disabled);
int ps2_noaudio(void);
/* Developer testing: boot.txt "eagerstream" decodes the music buffer a whole block at a time (the original behaviour) instead of in small
 * steps, so the two paths can be compared. */
void ps2_set_eagerstream(int eager);
int ps2_eagerstream(void);
/* Developer testing: boot.txt "openvideo" makes the Main Menu open Options > Video by itself (the game code asks via ps2_open_video()). */
void ps2_set_open_video(int on);
int ps2_open_video(void);
/* boot.txt "menu=Save_Select": open any menu (by its UIControl tag) by itself; returns the tag or NULL. */
void ps2_set_open_menu(const char *tag);
const char *ps2_open_menu(void);
/* Developer testing: boot.txt "audiocheck" logs a rolling checksum of the decoded music floats ([audiocheck] lines), identical in both decode
 * modes if the incremental decoder is bit-exact. Costs a little CPU, so it is off by default. */
void ps2_set_audiocheck(int on);
int ps2_audiocheck(void);
void ps2_set_achtest(int on);   /* boot.txt achtest: pop an achievement banner every ~10 s (exercises the string pool) */
int ps2_achtest(void);
void ps2_set_lod(int level);    /* boot.txt lod=0..3 pins the 3D detail level (faces smaller than that many pixels are dropped); -1 = adapt to the frame time */
int ps2_lod_pinned(void);
void ps2_set_lodcmp(int level); /* boot.txt lodcmp=N: a few times, draw the frame at detail 0 and at detail N from the same game state and log how many pixels differ */
int ps2_lodcmp(void);
void ps2_set_strstress(int on); /* boot.txt strstress: hammer the string pool (forces many defragmentations) and verify every string */
int ps2_strstress(void);
/* Developer tunables (boot.txt tune0=N .. tune7=N): ps2_tune(i, def) returns the override when one was given, else def. Lets scheduler constants be A/B tested without a rebuild.
 * 0 audio: ring free bytes above which mixing is forced (8192)   1 audio: percent of a slice's expected cost needed as free time (150)
 * 2 audio: extra free time needed, us (300)                      3 audio: forced slices per frame (4)
 * 4 3D detail: engine us above which detail drops (12000)        5 3D detail: engine us below which it comes back (9500)
 * 6 background half-rows: 1 = off (0)                            7 background half-rows: detail level that switches them on (4) */
void ps2_set_tune(int index, int value);
int ps2_tune(int index, int def);
void ps2_set_bot(int on);       /* boot.txt bot: after the title, holds RIGHT and jumps in a pattern (unattended long play-through test) */
void ps2_pad_read(int port, ps2_pad_state *out);

/* ---- Audio (audsrv): 16-bit stereo PCM queue ---- */
int ps2_audio_init(int rate);               /* loads the IOP modules, returns 1 on success */
void ps2_audio_lock(void);   /* recursive; LockAudioDevice() / UnlockAudioDevice() */
void ps2_audio_unlock(void);
int ps2_audio_is_paused(void);
int ps2_in_audio_thread(void);
int ps2_audio_thread_start(void (*step)(void)); /* starts the mixer thread; step() is called every few ms, returns 1 if the thread runs */
void ps2_audio_pause(void);              /* silence the output and stop mixing (call before something blocks for more than ~100 ms) */
void ps2_audio_resume(void);
void ps2_set_nopause(int on);  /* boot.txt nopause: developer A/B switch for the load pause */
int ps2_audio_free_bytes(void);              /* space left in the audsrv ring buffer */
void ps2_audio_write(const void *pcm, int bytes); /* queue interleaved S16 stereo (call only with <= free bytes) */

/* ---- Memory diagnostics ---- */

/* Bytes currently allocated from the C heap, and the EE RAM available to the program. */
uint32_t ps2_heap_used(void);
uint32_t ps2_ram_total(void);

#ifdef __cplusplus
}
#endif

#endif /* PS2_HAL_H */
