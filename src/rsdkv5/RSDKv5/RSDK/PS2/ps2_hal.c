/*
 * PS2 hardware abstraction layer: GS presentation, pad input, timing, memory diagnostics.
 * Derived from prototype p1_framebuffer (verified in PCSX2).
 */
#include "ps2_hal.h"

#include <kernel.h>
#include <debug.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <tamtypes.h>

#include <dmaKit.h>
#include <gsKit.h>
#include <libpad.h>
#include <audsrv.h>
#include <dirent.h>
#include <libmc.h>
#include <sys/stat.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <sbv_patches.h>

/*
 * EE stack size. crt0 uses the *address* of _stack_size as the size (it is a linker symbol), so it must
 * be an absolute symbol, not a variable. (An earlier "int _stack_size = ..." variable made crt0 reserve
 * ~4.5MB of stack and shrank the heap.)  256KB is a bit bigger than the 128KB default.
 */
__asm__(".globl _stack_size\n"
        ".set _stack_size, 0x40000\n");

static void diag_echo(const char *line); /* boot console, see below */
void ps2_user_log_flush_now(void); /* writes the buffered log.txt lines of a disc boot at once */
int ps2_in_audio_thread(void);

void ps2_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* EE SIO TX FIFO: PCSX2 forwards these bytes to its EE console log. */
    volatile unsigned char *sio = (volatile unsigned char *)0x1000F180; /* 0xB000F180 (the uncached kernel alias) raises an address error in user mode, which is how a real console runs the program */
    for (const char *p = buf; *p; ++p)
        *sio = (unsigned char)*p;
    if (ps2_in_audio_thread())
        return; /* the mixer thread only writes the serial console: the log buffer and the boot console are not thread safe */
    ps2_user_log_write(buf); /* disc boot: also kept in log.txt on the memory card (no host: or serial console on a real console) */
    diag_echo(buf);          /* boot console / crash screen */
}

/* ------------------------------------------------------------------------- */
/* Boot console and crash screen                                             */
/* ------------------------------------------------------------------------- */

/* A console has no serial log and no host: drive, so a game that fails to start just shows a black screen. The boot console is a text screen
 * (libdebug: the same one every PS2 homebrew uses, which works on any cable and in either TV standard) that lists the start-up steps while the game
 * loads. It stays up until the first game frame replaces it, so the last line on it says where a hang happened. If an exception happens at any time,
 * the crash screen shows the cause, the program counter and the last lines. Only a few kinds of log line are shown (see diag_wanted). */
#define PS2_DIAG_ROWS 12 /* log lines kept */
#define PS2_DIAG_COLS 64 /* characters kept per line (shown on up to two screen rows) */
#define PS2_DIAG_WRAP 32 /* characters per screen row of the crash screen (shown twice as large: 40 columns) */
#define PS2_CON_WRAP 46  /* the boot console is shown 1.5 times as large: 53 columns */
#define PS2_DIAG_HEAD 2  /* heading rows */
#define PS2_DIAG_BUDGET 11 /* text rows left for log lines on the boot console: 18 rows are visible, two are kept empty at the top (overscan), two are the heading */
static char s_diagLine[PS2_DIAG_ROWS][PS2_DIAG_COLS + 1];
static char s_diagHead[PS2_DIAG_HEAD][PS2_DIAG_COLS + 1];
static int s_diagCount; /* lines recorded so far; the newest PS2_DIAG_ROWS are kept */
static int s_diagOn;    /* 1 while the text console owns the display */
static int s_safeVideo = -1;
static const char *volatile s_stageText = "program start (nothing recorded yet)"; /* shown on the crash screen: the last step that began */
void ps2_stage(const char *text) { s_stageText = text; }
static int s_stdForce;             /* TV standard: 0 = the console's own (region), 1 = NTSC (hold L1 at start), 2 = PAL (hold R1); boot.txt std=ntsc|pal */
static int s_pal;                  /* the output in use is PAL 576i at 50 Hz */
static uint32_t s_vblankUs = 16683u; /* length of one display field in microseconds: NTSC 16683, PAL 20000 */
void ps2_set_tv_std(int std) { s_stdForce = std; }

static int diag_wanted(const char *s)
{
    static const char *const keep[] = { "[boot]", "[iop]",     "[audio]", "[save]",        "[usb]",      "[gfx]",         "[settings]",
                                        "[diag]", "[crash]", "[trace]", "[mem] render", "Loaded file", "File not found", "Loading Scene", "Main Menu", NULL };
    for (int i = 0; keep[i]; ++i) {
        if (strncmp(s, keep[i], strlen(keep[i])) == 0)
            return 1;
    }
    return 0;
}

/* The text screen (libdebug, 640x448 pixels, 8x8 characters) is far too small to read from a photograph. After init_scr() this makes the GS show
 * only its top-left 320x224 pixels, each drawn twice as wide and as tall (DISPLAY2 as libdebug sets it: DX 636, DY 50, MAGH 3, MAGV 1,
 * DW 2559, DH 447; here MAGH 7 and MAGV 3). The text then has 40 columns and about 26 rows, in characters twice the size. */
static void diag_magnify(int magh, int magv)
{
    const u64 display2 = 636ull | (50ull << 12) | ((u64)magh << 23) | ((u64)magv << 27) | (2559ull << 32) | (447ull << 44);
    *(volatile u64 *)0x120000A0 = display2;
}
#define DIAG_MAGNIFY_BIG() diag_magnify(7, 3)   /* 2x: 40 columns, 14 rows (crash screen) */
#define DIAG_MAGNIFY_MID() diag_magnify(5, 2)   /* 1.5x: 53 columns, 18 rows (boot console) */

/* prints a text wrapped to PS2_DIAG_WRAP characters per row, indented (a TV hides the first columns) */
static void diag_put_w(const char *text, int wrap);
static void diag_put(const char *text) { diag_put_w(text, PS2_DIAG_WRAP); }
static void diag_put_w(const char *text, int wrap)
{
    int pos = 0;
    for (int row = 0;; ++row) {
        scr_printf("%s%.*s\n", row ? "   " : " ", wrap, text + pos);
        const int left = (int)strlen(text + pos);
        if (left <= wrap)
            break;
        pos += wrap;
    }
}

static void diag_draw_all(void)
{
    scr_clear();
    scr_setXY(0, 3); /* a TV (and PCSX2) hides the first rows (overscan): keep the text inside */
    for (int i = 0; i < PS2_DIAG_HEAD; ++i) diag_put_w(s_diagHead[i], PS2_CON_WRAP);
    /* the newest lines that fit in the rows that are left (a line longer than one row takes two) */
    int rows = 0, first = s_diagCount;
    while (first > 0 && s_diagCount - first < PS2_DIAG_ROWS) {
        const int need = (int)strlen(s_diagLine[(first - 1) % PS2_DIAG_ROWS]) > PS2_CON_WRAP ? 2 : 1;
        if (rows + need > PS2_DIAG_BUDGET)
            break;
        rows += need;
        --first;
    }
    for (int i = first; i < s_diagCount; ++i) diag_put_w(s_diagLine[i % PS2_DIAG_ROWS], PS2_CON_WRAP);
}

static void diag_echo(const char *line)
{
    if (!diag_wanted(line))
        return;
    char *dst = s_diagLine[s_diagCount % PS2_DIAG_ROWS];
    int n     = 0;
    for (const char *c = line; *c && *c != '\n' && *c != '\r' && n < PS2_DIAG_COLS; ++c) dst[n++] = *c;
    dst[n] = 0;
    ++s_diagCount;
    if (s_diagOn)
        diag_draw_all();
}

/* Crash screen. The kernel calls ps2_exc_entry when an exception happens. Drawing from inside the exception does not work (the BIOS calls that set
 * up the text screen fail there), so the handler only notes the registers, points the exception's return address at ps2_crash_screen and returns:
 * the kernel then resumes "normally" in the crash screen, which draws the report and stops. Both entry points first move to a stack of their own (the
 * one that crashed may be the problem) and load the program's $gp (the kernel's is different, and small-data globals are read through it). */
/* The two stacks live in the 16 KB scratchpad (0x70000000, otherwise unused): ordinary arrays would take memory from the heap, which is full to
 * the last few KB. The exception entry gets the first 4 KB, the crash screen the rest. */
/* (the exception entry no longer returns to the kernel) */
unsigned ps2_crash_regs[4]; /* EPC, Cause, BadVAddr, Status */
unsigned ps2_exc_in[8];     /* registers as the kernel handed them to the handler: k0 k1 a0 a1 a2 ra sp gp (they may point at the saved context of the code that faulted) */
void ps2_exc_entry(void);
void ps2_crash_screen(void);
__asm__(".text\n"
        ".globl ps2_exc_entry\n"
        ".type ps2_exc_entry,@function\n"
        ".set push\n.set noreorder\n.set noat\n"
        "ps2_exc_entry:\n"
        "    lui   $at, %hi(ps2_exc_in)\n"
        "    addiu $at, $at, %lo(ps2_exc_in)\n"
        "    sw    $k0, 0($at)\n"
        "    sw    $k1, 4($at)\n"
        "    sw    $a0, 8($at)\n"
        "    sw    $a1, 12($at)\n"
        "    sw    $a2, 16($at)\n"
        "    sw    $ra, 20($at)\n"
        "    sw    $sp, 24($at)\n"
        "    sw    $gp, 28($at)\n"
        "    lui   $sp, 0x7000\n"
        "    ori   $sp, $sp, 0x0ff0\n"
        "    lui   $gp, %hi(_gp)\n"
        "    addiu $gp, $gp, %lo(_gp)\n"
        "    jal   ps2_exception_c\n"
        "    nop\n"
        "    j     ps2_crash_screen\n" /* ps2_exception_c has left the exception state: carry on in the crash screen */
        "    nop\n"
        ".globl ps2_crash_screen\n"
        ".type ps2_crash_screen,@function\n"
        "ps2_crash_screen:\n"
        "    lui   $sp, 0x7000\n"
        "    ori   $sp, $sp, 0x3ff0\n"
        "    lui   $gp, %hi(_gp)\n"
        "    addiu $gp, $gp, %lo(_gp)\n"
        "    j     ps2_crash_screen_c\n"
        "    nop\n"
        ".set pop\n");

static const char *const s_excNames[14] = { "interrupt",       "TLB modified",         "TLB miss (load/fetch)", "TLB miss (store)",
                                            "address error (load/fetch)", "address error (store)", "bus error (fetch)",   "bus error (data)",
                                            "syscall",         "breakpoint",           "reserved instruction", "coprocessor unusable",
                                            "overflow",        "trap" };

void ps2_exception_c(void)
{
    __asm__ volatile("mfc0 %0, $14" : "=r"(ps2_crash_regs[0]));
    __asm__ volatile("mfc0 %0, $13" : "=r"(ps2_crash_regs[1]));
    __asm__ volatile("mfc0 %0, $8" : "=r"(ps2_crash_regs[2]));
    __asm__ volatile("mfc0 %0, $12" : "=r"(ps2_crash_regs[3]));
    const unsigned code = (ps2_crash_regs[1] >> 2) & 31;
    ps2_log("[crash] exception %u (%s) EPC=%08x BadVAddr=%08x Cause=%08x Status=%08x\n", code, code < 14 ? s_excNames[code] : "other",
            ps2_crash_regs[0], ps2_crash_regs[2], ps2_crash_regs[1], ps2_crash_regs[3]);
    /* Leave the exception (clear Status.EXL) so that the crash screen runs as ordinary code, where the BIOS calls it needs work. The kernel's
     * own bookkeeping of the exception is abandoned: nothing returns from here. */
    unsigned st;
    __asm__ volatile("mfc0 %0, $12" : "=r"(st));
    st &= ~0x2u;
    __asm__ volatile("mtc0 %0, $12\n sync.p" : : "r"(st));
}

/* "00 41 E9 4C" as "0 0 4 1 E 9 4 C": spaced digits stay readable in a photograph of a blurry screen */
static void crash_hex(const char *label, unsigned v)
{
    char s[40];
    int n = 0;
    for (int i = 28; i >= 0; i -= 4) {
        s[n++] = "0123456789ABCDEF"[(v >> i) & 15];
        s[n++] = ' ';
        if (i == 16)
            s[n++] = ' ';
    }
    s[n] = 0;
    scr_printf("    %-8s%s\n", label, s);
}

/* prints the 4 words at an address when it is plausible (kernel RAM or program RAM, word aligned), else nothing */
static void crash_words(const char *label, unsigned addr)
{
    if ((addr & 3) || !((addr >= 0x80000000u && addr < 0x80100000u) || (addr >= 0x00100000u && addr < 0x01FFFFF0u)))
        return;
    const volatile unsigned *w = (const volatile unsigned *)addr;
    scr_printf("    [%s] %08x %08x %08x %08x\n", label, w[0], w[1], w[2], w[3]);
}

/* a line of text indented four columns (a TV hides the first three or four), wrapped at 42 characters */
static void crash_put(const char *text)
{
    int pos = 0;
    for (int row = 0;; ++row) {
        scr_printf("%s%.42s\n", row ? "      " : "    ", text + pos);
        if ((int)strlen(text + pos) <= 42)
            break;
        pos += 42;
    }
}

void ps2_crash_screen_c(void)
{
    const unsigned code = (ps2_crash_regs[1] >> 2) & 31;
    init_scr();
    scr_setbgcolor(0x000060); /* red for a crash */
    scr_setfontcolor(0xFFFFFF);
    scr_clear();
    DIAG_MAGNIFY_MID();
    scr_setXY(0, 3);
    char text[160];
    scr_printf("    PS2 CRASH built %s %s\n", __DATE__, __TIME__);
    snprintf(text, sizeof(text), "exc %u: %s", code, code < 14 ? s_excNames[code] : "other");
    crash_put(text);
    snprintf(text, sizeof(text), "stage: %s", s_stageText);
    crash_put(text);
    crash_hex("EPC", ps2_crash_regs[0]);
    crash_hex("BadVAddr", ps2_crash_regs[2]);
    crash_hex("Cause", ps2_crash_regs[1]);
    scr_printf("    k0 %08x k1 %08x a0 %08x\n", ps2_exc_in[0], ps2_exc_in[1], ps2_exc_in[2]);
    scr_printf("    a1 %08x a2 %08x ra %08x\n", ps2_exc_in[3], ps2_exc_in[4], ps2_exc_in[5]);
    {
        extern char _end[];
        scr_printf("    sp %08x heap %08x end %08x\n", ps2_exc_in[6], (unsigned)sbrk(0), (unsigned)_end);
    }
    crash_words("k0", ps2_exc_in[0]);
    crash_words("k1", ps2_exc_in[1]);
    crash_words("a0", ps2_exc_in[2]);
    crash_words("a1", ps2_exc_in[3]);
    scr_printf("    Please photograph this.\n");
    ps2_user_log_flush_now();
    for (;;) {
    }
}
static void diag_install_handlers(void)
{
    for (int c = 1; c <= 13; ++c) {
        if (c == 8) /* syscall: the kernel itself uses it */
            continue;
        SetVCommonHandler(c, (void *)ps2_exc_entry);
    }
    SetVTLBRefillHandler(2, (void *)ps2_exc_entry);
    SetVTLBRefillHandler(3, (void *)ps2_exc_entry);
}

/* Starts the text console and the crash screen. Call first thing in main(). */
static int s_diagStage; /* 0 = not started, 1 = text console up (from the first static initialiser), 2 = region logged (from main) */

void ps2_diag_init(void)
{
    if (s_diagStage == 1) {
        s_diagStage = 2;
        const int std = gsKit_detect_signal();
        ps2_log("[boot] console video standard: %s\n", std == GS_MODE_PAL ? "PAL (Europe)" : "NTSC");
        return;
    }
    if (s_diagStage)
        return;
    s_diagStage = 1;
    snprintf(s_diagHead[0], sizeof(s_diagHead[0]), "SONIC MANIA PS2 console");
    snprintf(s_diagHead[1], sizeof(s_diagHead[1]), "built %s %s", __DATE__, __TIME__);
    init_scr();
    DIAG_MAGNIFY_MID();
    scr_setbgcolor(0x500000); /* blue: a coloured screen tells "the game is running" from a black one */
    scr_setfontcolor(0xFFFFFF);
    s_diagOn = 1;
    diag_draw_all();
    diag_install_handlers();
}

/* Runs before the engine's own static initialisers (priority 150; those have the default, 65535), so that even a hang or crash while they run is visible. */
__attribute__((constructor(150))) static void ps2_early_start(void)
{
    ps2_stage("early: before the boot console");
    ps2_diag_init();
    ps2_stage("early: console up, before the first log line");
    ps2_log("[boot] program started: static initialisers are running\n");
    ps2_stage("early: first log line done, other static initialisers running");
}

void ps2_diag(const char *fmt, ...)
{
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char line[420];
    snprintf(line, sizeof(line), "[diag] %s\n", buf);
    ps2_log("%s", line);
}

/* A hand-over to the game's own video mode: the console goes away at the first game frame. */
static void diag_leave_console(const char *modeText)
{
    if (!s_diagOn)
        return;
    ps2_diag("starting game video: %s", modeText);
    ps2_audio_pause(); /* nothing refills the sound buffer during the wait: without this it would loop the last block as a buzz */
    usleep(1800 * 1000); /* long enough to read the last lines */
    ps2_audio_resume();
    s_diagOn = 0;
}

static int s_crashTest;
static void ps2_crash_now(void)
{
    __asm__ volatile("break 7"); /* breakpoint exception (an address error is not raised by PCSX2, a real console would) */
}
void ps2_set_crashtest(int mode)
{
    s_crashTest = mode;
    if (mode == 1) {
        ps2_log("[boot] crashtest: faulting now\n");
        ps2_crash_now();
    }
}
int ps2_safe_video(void)
{
    if (s_safeVideo >= 0)
        return s_safeVideo;
    s_safeVideo = 0;
    if (!ps2_pad_init())
        return 0;
    /* a pad needs a moment before it reports; stop waiting at the first report, or after about a second and a half when none is plugged in */
    for (int i = 0; i < 90; ++i) {
        ps2_pad_state st;
        ps2_pad_read(0, &st);
        if (st.connected) {
            s_safeVideo = (st.buttons & PS2_PAD_SELECT) ? 1 : 0;
            if (st.buttons & PS2_PAD_L1)
                s_stdForce = 1;
            else if (st.buttons & PS2_PAD_R1)
                s_stdForce = 2;
            break;
        }
        usleep(16 * 1000);
    }
    ps2_log("[boot] safe video (SELECT held at start): %s; TV standard override (L1 = NTSC, R1 = PAL): %s\n", s_safeVideo ? "YES: 480i" : "no", s_stdForce == 1 ? "NTSC" : (s_stdForce == 2 ? "PAL" : "none"));
    return s_safeVideo;
}

/* Finds the largest single block malloc can return right now (KB). */
static unsigned probe_max_block_kb(void)
{
    for (unsigned kb = 30 * 1024; kb >= 256; kb -= 128) {
        void *p = malloc((size_t)kb * 1024);
        if (p) {
            free(p);
            return kb;
        }
    }
    return 0;
}

void ps2_boot(void)
{
    SifInitRpc(0);
    ps2_log("[boot] Sonic Mania PS2 port prototype\n");
    (void)probe_max_block_kb; /* malloc probe is not meaningful here, see the sbrk probe below */
    {
        extern char _end[];
        char *start = (char *)sbrk(0);
        long grown = 0;
        while (sbrk(65536) != (void *)-1)
            grown += 65536;
        sbrk(-grown); /* hand it all back */
        int local;
        ps2_log("[boot] _end=%p sbrk0=%p max heap growth=%ld KB (heap limit ~%p) sp~%p\n", (void *)_end, start, grown / 1024, start + grown,
                (void *)&local);
    }
}

/* ------------------------------------------------------------------------- */
/* Graphics                                                                  */
/* ------------------------------------------------------------------------- */

static GSGLOBAL *s_gs;
static GSTEXTURE s_tex;
static u16 *s_gsFrame; /* ABGR1555 copy of the RSDK frame, 64-byte aligned */
static int s_texW, s_texH;

/* Display configuration (see ps2_gfx_configure). The GS video mode only changes when the effective output mode changes.
 *
 * Output modes:
 *   MODE_240P   the "240p" setting: the game is shown pixel perfect, 1:1, at its own resolution (320x240 for 4:3, 424x240 for 16:9), centred
 *               in a DTV 480p frame of 640x480 with black bars all around, so the picture is smaller than the screen. Nearest filter only;
 *               Letterbox and Scan mode do not apply. The output signal is always 480p, never the NTSC 240p mode.
 *   MODE_480I   NTSC, interlaced field mode, 704x480 (448 lines visible)
 *   MODE_480P   DTV 480p (component), 704x480
 * 704 is the "clean aperture" of the raster and a multiple of 64 as the GS frame buffer stride needs; 640 is too. "Scan mode" only chooses
 * between 480i and 480p when Resolution is 480; with 240p the output is 480p.
 *
 * In the 480 line modes the game's picture (424x240 for 16:9, 320x240 for 4:3) is stretched over the whole output frame:
 *   16:9, letterbox off  -> fills the frame; a widescreen TV stretches the frame back to 16:9 (anamorphic)
 *   16:9, letterbox on   -> shown at its true shape inside the 4:3 frame, black bars above and below (for 4:3 TVs)
 *   4:3                  -> fills the frame (letterbox has no effect, the picture is already 4:3) */
enum { PS2_MODE_240P = 0, PS2_MODE_480I = 1, PS2_MODE_480P = 2 };
static int s_cfgHires = 1;       /* 0 = 240p, 1 = 480 lines */
static int s_cfgProgressive = 0; /* with hires: 0 = 480i (field mode), 1 = 480p (DTV, component only: nothing shows on other cables) */
static int s_cfgWide = 0;        /* 0 = 4:3 picture (320x240), 1 = 16:9 picture (424x240) */
static int s_cfgLetterbox = 1;   /* with a 16:9 picture: 1 = true shape with black bars (4:3 TV), 0 = fill the frame (anamorphic) */
static int s_cfgLinear = 0;      /* 0 = clean (nearest), 1 = filtered (bilinear) */
static int s_ovrActive;          /* developer override (boot.txt vid=...) takes precedence over the game's settings */
static int s_curMode = -1;
static int s_outW = 704, s_outH = 240; /* current GS output size */
static int s_uOffset;                  /* first texel of the picture in the frame texture (see the MMI conversion) */
static int s_dmaReady;

/* The output mode the current settings ask for. 240p is always non-interlaced, so Scan mode only matters at 480 lines. */
static int gfx_wanted_mode(void)
{
    if (!s_cfgHires)
        return PS2_MODE_240P;
    return s_cfgProgressive ? PS2_MODE_480P : PS2_MODE_480I;
}

/* (Re)creates the GS in the configured video mode and the frame texture's VRAM. */
static void gfx_set_mode(void)
{
    /* A mode change keeps the gsKit object (queues, DMA buffers): only the mode fields below change and gsKit_init_screen() programs the new mode. Freeing and
     * allocating it again on every change (as this did before) is not what other programs do, and a console crashed in the kernel on the first frame of a
     * switch to 240p. Wait for the picture still being sent first. */
    if (s_dmaReady)
        dmaKit_wait_fast();

    /* gsKit's default render queues are 1MB (oneshot, double buffered = 2MB) + 256KB (persistent): ~2.3MB of the 32MB, all of it unused.
     * We draw one clear + one textured sprite per frame (a few hundred bytes of GIF packets); the frame texture upload builds its own
     * small DMA chain (gsKit_texture_send) and does not use these queues. 64KB / 16KB is generous. */
    if (!s_gs)
        s_gs = gsKit_init_global_custom(64 * 1024, 16 * 1024);

    const int wanted = gfx_wanted_mode();
    {
        static int detected = -1;
        if (detected < 0)
            detected = gsKit_detect_signal() == GS_MODE_PAL ? 1 : 0;
        s_pal = s_stdForce == 2 || (s_stdForce == 0 && detected);
        s_vblankUs = (wanted != PS2_MODE_480P && s_pal) ? 20000u : 16683u; /* the DTV modes run at 60 Hz whatever the console region */
    }
    switch (wanted) {
        default:
        case PS2_MODE_240P:
            /* A real 240p: the normal 15 kHz video signal (NTSC 60 Hz / PAL 50 Hz) with one 240-line field per refresh and no interlace. Works on every
             * cable, composite and SCART included. (This used to be a DTV 480p frame with the picture 1:1 in the middle, black all around, which only
             * a component cable shows and which went through the GetGsDxDyOffset syscall of gsKit.) */
            s_gs->Mode = s_pal ? GS_MODE_PAL : GS_MODE_NTSC;
            s_gs->Interlace = GS_NONINTERLACED;
            s_gs->Field = GS_FRAME;
            s_gs->Width = 640;
            s_gs->Height = 240;
            break;
        case PS2_MODE_480I:
            if (s_pal) { /* PAL console: 576i at 50 Hz (the NTSC picture shows without colour on a PAL TV fed through composite or SCART) */
                s_gs->Mode = GS_MODE_PAL;
                s_gs->Interlace = GS_INTERLACED;
                s_gs->Field = GS_FIELD;
                s_gs->Width = 640;
                s_gs->Height = 512;
                break;
            }
            s_gs->Mode = GS_MODE_NTSC;
            s_gs->Interlace = GS_INTERLACED;
            s_gs->Field = GS_FIELD;
            s_gs->Width = 704;
            s_gs->Height = 480;
            break;
        case PS2_MODE_480P:
            s_gs->Mode = GS_MODE_DTV_480P;
            s_gs->Interlace = GS_NONINTERLACED;
            s_gs->Field = GS_FRAME;
            s_gs->Width = 704;
            s_gs->Height = 480;
            break;
    }
    s_gs->PSM = GS_PSM_CT16;
    s_gs->PSMZ = GS_PSMZ_16S;
    s_gs->DoubleBuffering = GS_SETTING_ON;
    s_gs->ZBuffering = GS_SETTING_OFF;
    s_gs->PrimAlphaEnable = GS_SETTING_OFF;

    if (!s_dmaReady) {
        dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
        dmaKit_chan_init(DMA_CHANNEL_GIF);
        s_dmaReady = 1;
    }
    gsKit_init_screen(s_gs);
    gsKit_mode_switch(s_gs, GS_ONESHOT);

    s_outW = s_gs->Width;
    s_outH = s_gs->Height;
    s_curMode = wanted;

    s_tex.Vram = gsKit_vram_alloc(s_gs, gsKit_texture_size(s_tex.Width, s_tex.Height, s_tex.PSM), GSKIT_ALLOC_USERBUFFER);
    ps2_log("[gfx] TV standard: %s, %u us per field\n", (wanted != PS2_MODE_480P && s_pal) ? "PAL 50 Hz" : "60 Hz", (unsigned)s_vblankUs);
    ps2_log("[gfx] video mode: %s %dx%d  GS: MagH %d DW %d StartX %d  (aspect %s, letterbox %s, filter %s)\n",
            wanted == PS2_MODE_240P ? "240p" : (wanted == PS2_MODE_480P ? "480p" : "480i"), s_outW, s_outH, (int)s_gs->MagH, (int)s_gs->DW,
            (int)s_gs->StartX, s_cfgWide ? "16:9" : "4:3", s_cfgLetterbox ? "on" : "off", s_cfgLinear ? "filtered" : "clean");
}

static void (*s_idleHook)(void);

void ps2_gfx_set_idle_hook(void (*fn)(void)) { s_idleHook = fn; }

static void (*s_midHook)(void);
void ps2_gfx_set_mid_hook(void (*fn)(void)) { s_midHook = fn; }
/* Called from the middle of long stretches of engine work (between draw groups, layers and logic steps). The audio hook mixes only when the last mix is
 * overdue, i.e. when the frame is running long, so a slow frame cannot starve the sound buffer. */
void ps2_midframe(void)
{
    if (s_midHook)
        s_midHook();
}

/* Microseconds until the next vertical blank, estimated from the time the previous present returned (which is vblank-aligned). */
uint32_t ps2_vblank_remaining_us(void)
{
    extern uint32_t ps2_cycles(void);
    extern uint32_t ps2_prev_present_end(void);
    const uint32_t sinceUs = (uint32_t)((ps2_cycles() - ps2_prev_present_end()) / 295u);
    return s_vblankUs - (sinceUs % s_vblankUs);
}

void ps2_gfx_configure(int hires, int progressive, int widescreen, int letterbox, int filtered)
{
    if (s_ovrActive)
        return; /* boot.txt vid=... pins the settings (developer testing) */
    const int h = hires ? 1 : 0, p = progressive ? 1 : 0, w = widescreen ? 1 : 0, l = letterbox ? 1 : 0, f = filtered ? 1 : 0;
    static int s_logged = 0;
    if (!s_logged || h != s_cfgHires || p != s_cfgProgressive || w != s_cfgWide || l != s_cfgLetterbox || f != s_cfgLinear) {
        s_logged = 1;
        ps2_log("[gfx] settings: %s, scan %s, aspect %s, letterbox %s, filter %s\n", h ? "480 lines" : "240p", p ? "progressive" : "interlaced",
                w ? "16:9" : "4:3", l ? "on" : "off", f ? "filtered" : "clean");
    }
    s_cfgHires = h;
    s_cfgProgressive = p;
    s_cfgWide = w;
    s_cfgLetterbox = l;
    s_cfgLinear = f;
}

/* Developer testing: pins the display settings regardless of what the game's menu says. Spec: "480p:prog:169:lb1:filt" style fields are parsed
 * by the caller; this just applies them. */
void ps2_gfx_set_override(int hires, int progressive, int widescreen, int letterbox, int filtered)
{
    s_ovrActive = 0;
    ps2_gfx_configure(hires, progressive, widescreen, letterbox, filtered);
    s_ovrActive = 1;
}

/* True when the picture should be rendered 16:9 (424 wide), false for 4:3 (320 wide). Valid after ps2_gfx_configure. */
int ps2_gfx_wide(void) { return s_cfgWide; }

/* Developer testing: after `atFrame` presented frames, switch to another pinned display mode (boot.txt vidswap=...), to exercise changing the
 * settings while the game is running. */
static struct { uint32_t atFrame; int hires, progressive, wide, letterbox, filtered, armed; } s_swap;
static uint32_t s_swapFrames;

void ps2_gfx_schedule_swap(uint32_t atFrame, int hires, int progressive, int widescreen, int letterbox, int filtered)
{
    s_swap.atFrame = atFrame;
    s_swap.hires = hires;
    s_swap.progressive = progressive;
    s_swap.wide = widescreen;
    s_swap.letterbox = letterbox;
    s_swap.filtered = filtered;
    s_swap.armed = 1;
}

int ps2_gfx_init(int texW, int texH)
{
    s_texW = texW;
    s_texH = texH;

    size_t bytes = (size_t)texW * texH * sizeof(u16);
    s_gsFrame = (u16 *)memalign(64, bytes);
    if (!s_gsFrame)
        return 0;
    memset(s_gsFrame, 0, bytes);

    memset(&s_tex, 0, sizeof(s_tex));
    s_tex.Width = texW;
    s_tex.Height = texH;
    s_tex.PSM = GS_PSM_CT16;
    s_tex.Mem = (void *)s_gsFrame;
    s_tex.Filter = GS_FILTER_NEAREST;

    /* While the boot console is up the game's own video mode is not started: it would blank the text for the whole time the game still loads.
     * ps2_gfx_present() starts it with the first frame (it sees s_curMode != the wanted mode). */
    if (!s_diagOn)
        gfx_set_mode();
    return 1;
}
/* EE cycle counter (COP0 Count, 294.912 MHz, 32-bit). Subtracting two readings is wrap-safe for intervals under ~14 s. */
uint32_t ps2_cycles(void)
{
    uint32_t c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

#define PS2_PROF_FRAMES 300
#define PS2_CYCLES_PER_US 295u /* 294.912 MHz, rounded */
#define PS2_VBLANK_US s_vblankUs /* display field period: NTSC 16683 us, PAL 20000 us */

static struct {
    uint64_t engine, convert, queue, vsync;
    uint64_t slot[PS2_PROF_SLOTS]; /* engine sub-timings, accumulated by ps2_prof_add() */
    uint32_t lastEnd, frames;
    uint32_t vb1, vb2, vb3p; /* frames that took 1, 2, or 3+ vblanks (averages hide a few late frames) */
    uint32_t worstUs;        /* longest single frame, present to present */
    uint32_t work[8];        /* histogram of the CPU work per frame (engine + convert + queue + audio, without the vsync wait), see ps2_work_bucket() */
    uint32_t slowLogged;     /* [slow] lines printed in this window */
} s_prof;

/* Never cleared (s_prof is): lets the present function work out what a single frame spent in each slot. */
static uint64_t s_slotCum[PS2_PROF_SLOTS];

static int s_tune[8], s_tuneSet[8];
void ps2_set_tune(int index, int value)
{
    if (index >= 0 && index < 8) {
        s_tune[index]    = value;
        s_tuneSet[index] = 1;
    }
}
int ps2_tune(int index, int def) { return (index >= 0 && index < 8 && s_tuneSet[index]) ? s_tune[index] : def; }

void ps2_prof_add(int slot, uint32_t cycles)
{
    if (slot >= 0 && slot < PS2_PROF_SLOTS) {
        s_prof.slot[slot] += cycles;
        s_slotCum[slot] += cycles;
    }
}

#define PS2_PROF_CLASSES 64
static struct {
    const char *name;
    uint64_t cycles;
    uint32_t calls;
    uint32_t frameCycles; /* this frame only; cleared by the present function after the [slow] check */
} s_cls[PS2_PROF_CLASSES];

void ps2_prof_class(const char *name, uint32_t cycles)
{
    if (!name)
        return;
    for (int i = 0; i < PS2_PROF_CLASSES; ++i) {
        if (s_cls[i].name == name || s_cls[i].name == NULL) {
            s_cls[i].name = name;
            s_cls[i].cycles += cycles;
            s_cls[i].calls++;
            s_cls[i].frameCycles += cycles;
            return;
        }
    }
}

#define PS2_AUDIT_CLASSES 128
static struct {
    const char *name;
    uint32_t centred; /* Draw() calls where the entity's position was on screen */
    uint32_t drew;    /* ... of which put at least one pixel down */
} s_audit[PS2_AUDIT_CLASSES];

unsigned ps2_audit_seen(const char *name)
{
    for (int i = 0; i < PS2_AUDIT_CLASSES && s_audit[i].name; ++i)
        if (s_audit[i].name == name)
            return s_audit[i].centred;
    return 0;
}

static int s_auditEnabled = 1;

void ps2_audit_enable(int enabled) { s_auditEnabled = enabled; }

void ps2_audit_reset(void) { memset(s_audit, 0, sizeof(s_audit)); }

void ps2_audit_draw(const char *name, int centred, int drew)
{
    if (!name || !centred || !s_auditEnabled)
        return;
    for (int i = 0; i < PS2_AUDIT_CLASSES; ++i) {
        if (s_audit[i].name == name || s_audit[i].name == NULL) {
            s_audit[i].name = name;
            s_audit[i].centred++;
            if (drew)
                s_audit[i].drew++;
            return;
        }
    }
}

/* Cumulative since boot. Lists every class that was on screen but did NOT draw on every appearance (worst first), then a summary of
 * how many classes drew every time. A class with a Draw() that is on screen yet puts no pixels down is a candidate for a missing sprite. */
void ps2_audit_dump(void)
{
    int seen = 0, perfect = 0;
    int printed[PS2_AUDIT_CLASSES] = { 0 };
    ps2_log("[audit] ---- classes that were on screen but did not always draw (drew / on-screen) ----\n");
    for (int rank = 0; rank < PS2_AUDIT_CLASSES; ++rank) {
        int best = -1;
        for (int i = 0; i < PS2_AUDIT_CLASSES && s_audit[i].name; ++i) {
            if (printed[i] || s_audit[i].drew >= s_audit[i].centred)
                continue;
            if (best < 0 || s_audit[i].drew * s_audit[best].centred < s_audit[best].drew * s_audit[i].centred)
                best = i;
        }
        if (best < 0)
            break;
        printed[best] = 1;
        ps2_log("[audit]   %-26s drew %6u / %6u  (%u%%)\n", s_audit[best].name, (unsigned)s_audit[best].drew, (unsigned)s_audit[best].centred,
                (unsigned)(s_audit[best].drew * 100u / s_audit[best].centred));
    }
    for (int i = 0; i < PS2_AUDIT_CLASSES && s_audit[i].name; ++i) {
        ++seen;
        if (s_audit[i].drew >= s_audit[i].centred)
            ++perfect;
    }
    ps2_log("[audit] summary: %d classes seen on screen, %d drew every time, %d did not always draw\n", seen, perfect, seen - perfect);
}

static void ps2_audit_report(void)
{
    ps2_audit_dump();
}

static void ps2_prof_class_report(uint32_t frames)
{
    /* Print the 9 most expensive classes (selection by repeated max), then clear. */
    for (int rank = 0; rank < 9; ++rank) {
        int best = -1;
        for (int i = 0; i < PS2_PROF_CLASSES && s_cls[i].name; ++i)
            if (s_cls[i].cycles && (best < 0 || s_cls[i].cycles > s_cls[best].cycles))
                best = i;
        if (best < 0)
            break;
        ps2_log("[prof]     class %-24s %6u us/frame  (%u draws/frame)\n", s_cls[best].name,
                (unsigned)(s_cls[best].cycles / frames / PS2_CYCLES_PER_US), (unsigned)(s_cls[best].calls / frames));
        s_cls[best].cycles = 0;
    }
    memset(s_cls, 0, sizeof(s_cls));
}

static uint32_t s_prevEnd;     /* cycle count at the end of the previous present */
static uint32_t s_frameCycles; /* length of the last whole frame in EE cycles */
static uint32_t s_stepsNext = 1; /* 60 Hz logic steps for the next frame, see ps2_logic_steps */
static uint32_t s_stepAcc;       /* PAL: fifths of a step carried to the next frame */

/* How many 60 Hz logic steps the next frame should run so game speed stays real-time when drawing is slower than one vblank.
 * Frames are quantised to whole vblanks by the vsync wait, so rounding to the nearest vblank count is exact. Capped at 3 so a
 * slow scene (or a long load) cannot make the logic cost snowball. */
uint32_t ps2_logic_steps(void) { return s_stepsNext; }

static void ps2_prof_report(void)
{
    uint32_t n   = s_prof.frames ? s_prof.frames : 1;
    uint32_t eng = (uint32_t)(s_prof.engine / n / PS2_CYCLES_PER_US);
    uint32_t cnv = (uint32_t)(s_prof.convert / n / PS2_CYCLES_PER_US);
    uint32_t que = (uint32_t)(s_prof.queue / n / PS2_CYCLES_PER_US);
    uint32_t vsy = (uint32_t)(s_prof.vsync / n / PS2_CYCLES_PER_US);
    uint32_t tot = eng + cnv + que + vsy;

    /* engine = logic + software rasteriser (time between present calls); vsync = idle wait for the display. */
    ps2_log("[prof] avg/frame over %u: engine=%u us convert=%u us queue=%u us vsync-wait=%u us total=%u us (%u.%02u vblanks/frame)\n", n, eng, cnv,
            que, vsy, tot, tot * 100u / PS2_VBLANK_US / 100u, tot * 100u / PS2_VBLANK_US % 100u);
    ps2_log("[prof]   pacing: %u frames on time (1 vblank), %u took 2, %u took 3+, worst frame %u us\n", (unsigned)s_prof.vb1, (unsigned)s_prof.vb2,
            (unsigned)s_prof.vb3p, (unsigned)s_prof.worstUs);
    ps2_log("[prof]   work time per frame (ms, CPU only): <8:%u 8-10:%u 10-12:%u 12-14:%u 14-16:%u 16-18:%u 18-22:%u 22+:%u\n", (unsigned)s_prof.work[0],
            (unsigned)s_prof.work[1], (unsigned)s_prof.work[2], (unsigned)s_prof.work[3], (unsigned)s_prof.work[4], (unsigned)s_prof.work[5],
            (unsigned)s_prof.work[6], (unsigned)s_prof.work[7]);
    /* Sub-timings of "engine": object update (logic) vs draw lists (software rasteriser), and what the draw time is made of.
     * All values are per-frame averages in microseconds; layers + entities are parts of draw. */
    {
        uint32_t logic = (uint32_t)(s_prof.slot[PS2_PROF_LOGIC] / n / PS2_CYCLES_PER_US);
        uint32_t draw  = (uint32_t)(s_prof.slot[PS2_PROF_DRAW] / n / PS2_CYCLES_PER_US);
        uint32_t lay   = (uint32_t)(s_prof.slot[PS2_PROF_LAYERS] / n / PS2_CYCLES_PER_US);
        uint32_t ent   = (uint32_t)(s_prof.slot[PS2_PROF_ENTITIES] / n / PS2_CYCLES_PER_US);
        uint32_t other = eng > logic + draw ? eng - logic - draw : 0;
        ps2_log("[prof]   engine split: logic=%u draw=%u (tile-layers=%u entities=%u) other=%u us, logic steps now=%u\n", logic, draw, lay, ent,
                other, (unsigned)ps2_logic_steps());
        ps2_prof_class_report(n);

        /* The audit is cumulative; print it every 3rd profile window (~15 s) to keep the console readable. */
        static uint32_t s_auditTick;
        if ((++s_auditTick % 3) == 0)
            ps2_audit_report();
    }
    memset(&s_prof, 0, sizeof(s_prof));
}

uint32_t ps2_prev_present_end(void) { return s_prevEnd; }

static const uint32_t s_mmiMask1F[4] __attribute__((aligned(16))) = { 0x001F001Fu, 0x001F001Fu, 0x001F001Fu, 0x001F001Fu };
static const uint32_t s_mmiMask3E0[4] __attribute__((aligned(16))) = { 0x03E003E0u, 0x03E003E0u, 0x03E003E0u, 0x03E003E0u };
static const uint32_t s_mmiMaskAlpha[4] __attribute__((aligned(16))) = { 0x80008000u, 0x80008000u, 0x80008000u, 0x80008000u };

/*
 * Frame texture upload without heap allocation.
 *
 * gsKit_texture_upload() -> gsKit_texture_send() memalign()s its DMA chain on every call, sizes it by the whole image's quadword
 * count (about 262KB for our 512x256 frame, although the chain itself needs ~10 quadwords because the pixel data is referenced, not
 * copied), and never checks for NULL. With the heap almost full (a few KB free) the allocation can fail right after a scene load
 * and the library then stores through NULL (PCSX2 logged 48 "TLB Miss, pc=gsKit_texture_send addr=0x0..0x48 [store]" per act load).
 * On real hardware that is a write to low memory, not a harmless log line.
 *
 * This builds the same chain (same tags, same trailing TEXFLUSH as gsKit's GS_CLUT_NONE path) in a static buffer instead.
 */
#define PS2_UPLOAD_MAX_QW 64
static u64 s_uploadChain[PS2_UPLOAD_MAX_QW * 2] __attribute__((aligned(64)));

static void ps2_texture_upload(GSTEXTURE *t)
{
    gsKit_setup_tbw(t);

    u32 qwc = gsKit_texture_size_ee(t->Width, t->Height, t->PSM) / 16;
    if (gsKit_texture_size_ee(t->Width, t->Height, t->PSM) % 16)
        qwc++;
    const int packets = (int)(qwc / GS_GIF_BLOCKSIZE);
    const int remain = (int)(qwc % GS_GIF_BLOCKSIZE);

    /* 6 qw header + 3 qw per image block + 3 qw trailer. */
    const int blocks = packets + (remain > 0 ? 1 : 0);
    const int chainQw = 6 + blocks * 3 + 3;
    if (chainQw > PS2_UPLOAD_MAX_QW) {
        static int warned;
        if (!warned) {
            warned = 1;
            ps2_log("[gfx] frame texture needs %d qw of DMA chain, max %d: upload skipped\n", chainQw, PS2_UPLOAD_MAX_QW);
        }
        return;
    }

    u64 *p = s_uploadChain;
    const u32 *mem = (const u32 *)t->Mem;

    *p++ = DMA_TAG(5, 0, DMA_CNT, 0, 0, 0);
    *p++ = 0;
    *p++ = GIF_TAG(4, 0, 0, 0, GSKIT_GIF_FLG_PACKED, 1);
    *p++ = GIF_AD;
    *p++ = GS_SETREG_BITBLTBUF(0, 0, 0, t->Vram / 256, t->TBW, t->PSM);
    *p++ = GS_BITBLTBUF;
    *p++ = GS_SETREG_TRXPOS(0, 0, 0, 0, 0);
    *p++ = GS_TRXPOS;
    *p++ = GS_SETREG_TRXREG(t->Width, t->Height);
    *p++ = GS_TRXREG;
    *p++ = GS_SETREG_TRXDIR(0);
    *p++ = GS_TRXDIR;

    for (int i = 0; i < packets; ++i) {
        *p++ = DMA_TAG(1, 0, DMA_CNT, 0, 0, 0);
        *p++ = 0;
        *p++ = GIF_TAG(GS_GIF_BLOCKSIZE, 0, 0, 0, GSKIT_GIF_FLG_IMAGE, 0);
        *p++ = 0;
        *p++ = DMA_TAG(GS_GIF_BLOCKSIZE, 1, DMA_REF, 0, (u32)mem, 0);
        *p++ = 0;
        mem += GS_GIF_BLOCKSIZE * 4;
    }
    if (remain > 0) {
        *p++ = DMA_TAG(1, 0, DMA_CNT, 0, 0, 0);
        *p++ = 0;
        *p++ = GIF_TAG(remain, 0, 0, 0, GSKIT_GIF_FLG_IMAGE, 0);
        *p++ = 0;
        *p++ = DMA_TAG(remain, 1, DMA_REF, 0, (u32)mem, 0);
        *p++ = 0;
    }

    *p++ = DMA_TAG(2, 0, DMA_END, 0, 0, 0);
    *p++ = 0;
    *p++ = GIF_TAG(1, 1, 0, 0, GSKIT_GIF_FLG_PACKED, 1);
    *p++ = GIF_AD;
    *p++ = 0;
    *p++ = GS_TEXFLUSH;

    /* Wait for any draw still in flight, send, then wait again so the static chain is not rewritten while the DMA reads it. */
    dmaKit_wait_fast();
    dmaKit_send_chain(DMA_CHANNEL_GIF, s_uploadChain, (u32)chainQw);
    dmaKit_wait_fast();
}

static uint32_t s_burnUs; /* developer (boot.txt burn=MS): waste this much time every frame, to imitate a console slower than the emulator */
void ps2_set_burn_ms(int ms) { s_burnUs = ms > 0 ? (uint32_t)ms * 1000u : 0u; }

void ps2_gfx_present(const uint16_t *src, int pitchPixels, int width, int height)
{
    if (s_burnUs) {
        const uint32_t b0 = ps2_cycles();
        while ((ps2_cycles() - b0) / PS2_CYCLES_PER_US < s_burnUs) {
        }
    }
    static const u64 black = GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00);
    static const u64 white = GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00);

    const uint32_t tStart = ps2_cycles();
    const uint32_t engineCycles = s_prof.lastEnd ? (uint32_t)(tStart - s_prof.lastEnd) : 0; /* logic + software rasteriser of this frame */
    if (s_prof.lastEnd)
        s_prof.engine += (uint32_t)(tStart - s_prof.lastEnd);

    {
        static int once;
        if (!once++)
            ps2_log("[gfx] present: src=%p (align %u) pitch=%d width=%d height=%d dst=%p\n", (const void *)src, (unsigned)((uintptr_t)src & 15), pitchPixels, width, height, (void *)s_gsFrame);
    }
    if (width > s_texW)
        width = s_texW;
    if (height > s_texH)
        height = s_texH;

    /* RGB565 -> GS CT16 (ABGR1555, alpha bit set).
     * Two pixels per 32-bit word. RGB565 = RRRRRGGGGGGBBBBB, GS CT16 = ABBBBBGGGGGRRRRR (we keep the top 5 bits of green):
     *   R = (w >> 11) & 0x001F001F      G = (w >> 1) & 0x03E003E0      B = (w & 0x001F001F) << 10      A = 0x80008000
     * The masks keep the two pixels in the word from leaking into each other. Rows that are not 4-byte aligned use the scalar loop. */
    /* MMI loads/stores need 16-byte alignment. The source rows are only 8-byte aligned (the buffer lives inside a struct), so start each row
     * at the previous 16-byte boundary and write the pixels shifted by the same amount; the sprite then samples from texel u = shift. */
    int mmiShift = 0, useMmi = 0;
    if ((((uintptr_t)src & 1) == 0) && (((size_t)pitchPixels * 2) & 15) == 0 && (((uintptr_t)s_gsFrame) & 15) == 0 && ((s_texW * 2) & 15) == 0) {
        mmiShift = (int)(((uintptr_t)src & 15) >> 1);
        useMmi = (((width + mmiShift + 7) >> 3) << 3) <= s_texW;
        if (!useMmi)
            mmiShift = 0;
    }
    s_uOffset = mmiShift;

    for (int y = 0; y < height; ++y) {
        const u16 *row = src + (size_t)y * pitchPixels;
        u16 *dst = s_gsFrame + (size_t)y * s_texW;
        int x = 0;

        /* Fast path: 128-bit MMI, 8 pixels per iteration (psrlw/psllw/pand/por act on four 32-bit words at once, i.e. exactly the two-pixels-per-
         * word trick below, four words per instruction). Needs 16-byte aligned rows. */
        if (useMmi) {
            const u128 *in = (const u128 *)(row - mmiShift);
            u128 *out = (u128 *)dst;
            const int octs = (width + mmiShift + 7) >> 3;
            u128 m1f, m3e0, ma;
            __asm__ volatile("lq %0, 0(%1)" : "=r"(m1f) : "r"(s_mmiMask1F));
            __asm__ volatile("lq %0, 0(%1)" : "=r"(m3e0) : "r"(s_mmiMask3E0));
            __asm__ volatile("lq %0, 0(%1)" : "=r"(ma) : "r"(s_mmiMaskAlpha));
            for (int i = 0; i < octs; ++i) {
                u128 w, r, g, b;
                __asm__ volatile("lq %0, 0(%1)" : "=r"(w) : "r"(in + i));
                __asm__ volatile("psrlw %0, %1, 11" : "=r"(r) : "r"(w));
                __asm__ volatile("psrlw %0, %1, 1" : "=r"(g) : "r"(w));
                __asm__ volatile("pand %0, %1, %2" : "=r"(b) : "r"(w), "r"(m1f));
                __asm__ volatile("pand %0, %1, %2" : "=r"(r) : "r"(r), "r"(m1f));
                __asm__ volatile("pand %0, %1, %2" : "=r"(g) : "r"(g), "r"(m3e0));
                __asm__ volatile("psllw %0, %1, 10" : "=r"(b) : "r"(b));
                __asm__ volatile("por %0, %1, %2" : "=r"(r) : "r"(r), "r"(g));
                __asm__ volatile("por %0, %1, %2" : "=r"(b) : "r"(b), "r"(ma));
                __asm__ volatile("por %0, %1, %2" : "=r"(r) : "r"(r), "r"(b));
                __asm__ volatile("sq %0, 0(%1)" : : "r"(r), "r"(out + i) : "memory");
            }
            x = width; /* whole row done */
        } else if ((((uintptr_t)row | (uintptr_t)dst) & 3) == 0) {
            const uint32_t *in = (const uint32_t *)row;
            uint32_t *out = (uint32_t *)dst;
            const int pairs = width >> 1;
            int i = 0;
            for (; i + 4 <= pairs; i += 4) {
                uint32_t w0 = in[i], w1 = in[i + 1], w2 = in[i + 2], w3 = in[i + 3];
                out[i]     = 0x80008000u | ((w0 >> 11) & 0x001F001Fu) | ((w0 >> 1) & 0x03E003E0u) | ((w0 & 0x001F001Fu) << 10);
                out[i + 1] = 0x80008000u | ((w1 >> 11) & 0x001F001Fu) | ((w1 >> 1) & 0x03E003E0u) | ((w1 & 0x001F001Fu) << 10);
                out[i + 2] = 0x80008000u | ((w2 >> 11) & 0x001F001Fu) | ((w2 >> 1) & 0x03E003E0u) | ((w2 & 0x001F001Fu) << 10);
                out[i + 3] = 0x80008000u | ((w3 >> 11) & 0x001F001Fu) | ((w3 >> 1) & 0x03E003E0u) | ((w3 & 0x001F001Fu) << 10);
            }
            for (; i < pairs; ++i) {
                uint32_t w = in[i];
                out[i] = 0x80008000u | ((w >> 11) & 0x001F001Fu) | ((w >> 1) & 0x03E003E0u) | ((w & 0x001F001Fu) << 10);
            }
            x = pairs << 1;
        }

        for (; x < width; ++x) {
            u16 c = row[x];
            u16 r = (c >> 11) & 0x1F;
            u16 g = (c >> 6) & 0x1F;
            u16 b = c & 0x1F;
            dst[x] = 0x8000 | (b << 10) | (g << 5) | r;
        }
    }
    FlushCache(0);
    ps2_stage("frame: GS upload and display");
    const uint32_t tConverted = ps2_cycles();

    if (s_swap.armed && ++s_swapFrames >= s_swap.atFrame) {
        s_swap.armed = 0;
        ps2_log("[gfx] scheduled display swap now (frame %u)\n", (unsigned)s_swapFrames);
        ps2_gfx_set_override(s_swap.hires, s_swap.progressive, s_swap.wide, s_swap.letterbox, s_swap.filtered);
    }

    if (s_crashTest == 2) {
        static int crashFrames;
        if (++crashFrames == 240) {
            ps2_log("[boot] crashtest: faulting now\n");
            ps2_crash_now();
        }
    }
    /* Switch the GS output mode if Resolution / Scan mode changed (the first frame starts the game's video; the boot console goes away). */
    if (gfx_wanted_mode() != s_curMode) {
        const int wm = gfx_wanted_mode();
        diag_leave_console(wm == PS2_MODE_240P ? "240p (works on every cable)" : (wm == PS2_MODE_480P ? "480p progressive (component only)" : "480i interlaced (works on every cable)"));
        gfx_set_mode();
    }

    /* Display rectangle inside the output frame (see the mode notes at the top). The frame itself is a 4:3 picture area: a 4:3 TV shows it
     * as is, a widescreen TV stretches it to 16:9. So:
     *   - a 16:9 picture with the letterbox on is drawn at its true shape inside the frame (bars above and below), for 4:3 TVs;
     *   - everything else fills the whole frame (16:9 anamorphic, or a 4:3 picture, which already has the frame's shape). */
    /* NTSC 480i shows 448 of the 480 lines the GS is asked for: PCSX2 (like a real NTSC set) cuts 16 rows off the top and
     * the bottom, which took the top and bottom 8 lines of the game's 240 away (HUD and title text clipped). The picture is therefore fitted
     * into the 448 visible lines and centred; the rows outside stay black. 480p (DTV) shows all 480 lines. Measured in PCSX2 with
     * tools/display_check.ps1: the 16:9 letterbox picture covered 81% of the window height (362 of 448 rows) instead of 76% (362 of 480). */
    /* NTSC sets show about 448 of the 480 lines of a 480i frame and about 224 of the 240 of a 240p one (rows at the top and bottom are hidden) */
    const float visH = (s_curMode == PS2_MODE_480I) ? (s_pal ? 480.0f : 448.0f) : ((s_curMode == PS2_MODE_240P && !s_pal) ? 224.0f : (float)s_outH);
    const float c = (float)width / (float)height;
    float dispW = (float)s_outW, dispH = visH;
    float x0, y0;
    const int pixelPerfect = 0; /* (the old DTV 240p drew 1:1 with black all around; 240p now fills its frame like the other modes) */
    if (pixelPerfect) {
        /* 240p: the picture is drawn 1:1 (one game pixel = one output pixel), centred at whole-pixel offsets with black all around.
         * Letterbox, Scan mode and Filter do not apply. */
        dispW = (float)width;
        dispH = (float)height;
        x0 = (float)((s_outW - width) / 2);
        y0 = (float)((s_outH - height) / 2);
    }
    else {
        if (s_cfgLetterbox && c > (4.0f / 3.0f) + 0.01f)
            dispH = visH * (4.0f / 3.0f) / c;
        x0 = ((float)s_outW - dispW) * 0.5f;
        y0 = ((float)s_outH - dispH) * 0.5f;
    }

    /* Clean = nearest neighbour. Filtered = bilinear; the source rectangle is inset by half a texel on every side so the filter never blends
     * in texels outside the picture (the frame texture is larger than the picture and holds stale pixels beyond it). */
    const int linear = s_cfgLinear && !pixelPerfect;
    s_tex.Filter = linear ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
    const float inset = linear ? 0.5f : 0.0f;

    gsKit_clear(s_gs, black);
    ps2_texture_upload(&s_tex);
    gsKit_prim_sprite_texture(s_gs, &s_tex, x0, y0, (float)s_uOffset + inset, inset, x0 + dispW, y0 + dispH, (float)(width + s_uOffset) - inset,
                              (float)height - inset, 1, white);
    gsKit_queue_exec(s_gs);
    const uint32_t tQueued = ps2_cycles();

    /* The EE would otherwise spin in the vsync wait: use that slack for work that can happen any time (audio mixing). */
    const uint32_t tIdle0 = ps2_cycles();
    if (s_idleHook)
        s_idleHook();
    const uint32_t idleCycles = ps2_cycles() - tIdle0; /* audio mixing (the idle hook) */

    gsKit_sync_flip(s_gs);
    const uint32_t tEnd = ps2_cycles();

    /* Pacing: remember how long this whole frame took (previous present -> this one) for ps2_logic_steps(). Kept outside
     * s_prof because the profiler report clears that struct. */
    if (s_prevEnd) {
        s_frameCycles = tEnd - s_prevEnd;
        {
            /* Logic always runs at 60 steps per second. NTSC: one step per display field. PAL has 50 fields a second, so 6 steps per 5 fields
             * (1,1,1,1,2). A frame that took several fields runs that many fields' worth of steps; at most 3, so a slow scene or a long load
             * cannot make the logic cost snowball. */
            uint32_t fields = (s_frameCycles / PS2_CYCLES_PER_US + s_vblankUs / 2) / s_vblankUs;
            if (fields < 1)
                fields = 1;
            s_stepAcc += fields * (s_vblankUs > 18000u ? 6u : 5u);
            uint32_t st = s_stepAcc / 5u;
            s_stepAcc %= 5u;
            if (st > 3u) {
                st = 3u;
                s_stepAcc = 0;
            }
            s_stepsNext = st < 1u ? 1u : st;
        }
        const uint32_t frameUs = s_frameCycles / PS2_CYCLES_PER_US;
        const uint32_t vb = (frameUs + PS2_VBLANK_US / 2) / PS2_VBLANK_US;
        if (vb <= 1)
            s_prof.vb1++;
        else if (vb == 2)
            s_prof.vb2++;
        else
            s_prof.vb3p++;
        if (frameUs > s_prof.worstUs)
            s_prof.worstUs = frameUs;
    }
    s_prevEnd = tEnd;

    {
        /* Per-frame breakdown. The work time is everything the CPU did for this frame; vsync wait is idle. A frame that took 2+ vblanks although
         * the work time is below one vblank points at a synchronisation problem rather than at too much work. */
        static uint64_t s_prevCum[PS2_PROF_SLOTS];
        static uint32_t s_frameNo;
        ++s_frameNo;
        uint32_t d[PS2_PROF_SLOTS];
        for (int i = 0; i < PS2_PROF_SLOTS; ++i) {
            d[i]       = (uint32_t)(s_slotCum[i] - s_prevCum[i]);
            s_prevCum[i] = s_slotCum[i];
        }
        const uint32_t convC = tConverted - tStart, queC = tQueued - tConverted;
        const uint32_t workUs = (engineCycles + convC + queC + idleCycles) / PS2_CYCLES_PER_US;
        const uint32_t frameUs = s_frameCycles / PS2_CYCLES_PER_US;
        static const uint32_t edges[7] = { 8000, 10000, 12000, 14000, 16000, 18000, 22000 };
        int b = 0;
        while (b < 7 && workUs >= edges[b])
            ++b;
        s_prof.work[b]++;
        if (frameUs > 22000 && s_prof.slowLogged < 10 && s_frameNo > 300) {
            ++s_prof.slowLogged;
            int a = -1, c = -1;
            for (int i = 0; i < PS2_PROF_CLASSES && s_cls[i].name; ++i) {
                if (!s_cls[i].frameCycles)
                    continue;
                if (a < 0 || s_cls[i].frameCycles > s_cls[a].frameCycles) {
                    c = a;
                    a = i;
                }
                else if (c < 0 || s_cls[i].frameCycles > s_cls[c].frameCycles)
                    c = i;
            }
            const uint32_t eng = engineCycles / PS2_CYCLES_PER_US, logic = d[PS2_PROF_LOGIC] / PS2_CYCLES_PER_US, draw = d[PS2_PROF_DRAW] / PS2_CYCLES_PER_US;
            ps2_log("[slow] f=%u frame=%u us work=%u us | engine=%u (logic=%u draw=%u [tiles=%u ents=%u] other=%u) convert=%u queue=%u audio=%u | steps=%u top: %s=%u %s=%u\n",
                    (unsigned)s_frameNo, (unsigned)frameUs, (unsigned)workUs, (unsigned)eng, (unsigned)logic, (unsigned)draw,
                    (unsigned)(d[PS2_PROF_LAYERS] / PS2_CYCLES_PER_US), (unsigned)(d[PS2_PROF_ENTITIES] / PS2_CYCLES_PER_US),
                    (unsigned)(eng > logic + draw ? eng - logic - draw : 0), (unsigned)(convC / PS2_CYCLES_PER_US), (unsigned)(queC / PS2_CYCLES_PER_US),
                    (unsigned)(idleCycles / PS2_CYCLES_PER_US), (unsigned)ps2_logic_steps(), a >= 0 ? s_cls[a].name : "-",
                    a >= 0 ? (unsigned)(s_cls[a].frameCycles / PS2_CYCLES_PER_US) : 0u, c >= 0 ? s_cls[c].name : "-",
                    c >= 0 ? (unsigned)(s_cls[c].frameCycles / PS2_CYCLES_PER_US) : 0u);
        }
        for (int i = 0; i < PS2_PROF_CLASSES && s_cls[i].name; ++i)
            s_cls[i].frameCycles = 0;
    }

    s_prof.convert += (uint32_t)(tConverted - tStart);
    s_prof.queue += (uint32_t)(tQueued - tConverted);
    s_prof.vsync += (uint32_t)(tEnd - tQueued);
    s_prof.lastEnd = tEnd;
    if (++s_prof.frames >= PS2_PROF_FRAMES)
        ps2_prof_report();
}

void ps2_gfx_shutdown(void) {}

/* ------------------------------------------------------------------------- */
/* Timing                                                                    */
/* ------------------------------------------------------------------------- */

uint64_t ps2_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

/* ------------------------------------------------------------------------- */
/* Pad                                                                       */
/* ------------------------------------------------------------------------- */

static char s_padBuf[2][256] __attribute__((aligned(64)));
static int s_padOpen[2];     /* port opened with padman */
static int s_padModeSet[2];  /* analog DualShock mode requested (done once a pad is actually plugged in, so hot-plugging works) */

static void ps2_iop_sio2(void); /* loads rom0:SIO2MAN once; shared by the pad and memory card code */

/* Opens both controller ports. Port 2 may be empty: it is polled later and starts working when a pad shows up. Returns 1 if port 1 opened. */
static int s_padInitDone = -1; /* result of the first ps2_pad_init (the safe-video check runs it before the engine does) */
int ps2_pad_init(void)
{
    if (s_padInitDone >= 0)
        return s_padInitDone;
    s_padInitDone = 0;
    SifInitRpc(0);
    ps2_iop_sio2();
    SifLoadModule("rom0:PADMAN", 0, NULL);
    padInit(0);
    for (int port = 0; port < 2; ++port) {
        if (padPortOpen(port, 0, s_padBuf[port]))
            s_padOpen[port] = 1;
    }
    if (!s_padOpen[0])
        return 0;
    s_padInitDone = 1;

    /* Port 1: wait for the pad to settle (bounded). Port 2 is not waited for, there may be no pad in it. */
    int state;
    int guard = 0;
    do {
        state = padGetState(0, 0);
    } while (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1 && ++guard < 1000000);
    return 1;
}

/* Requests analog DualShock mode (sticks report real values) the first time a pad on this port is stable. */
static void ps2_pad_setup_mode(int port)
{
    if (s_padModeSet[port])
        return;
    s_padModeSet[port] = 1;
    padSetMainMode(port, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
}

/* ---- Scripted input (developer testing) ----
 * boot.txt "inject=FRAME:BUTTON:FRAMES/FRAME:BUTTON:FRAMES/..." presses pad buttons on port 0 at the given frame counts (counted in pad polls,
 * i.e. engine frames since boot) for the given number of frames, e.g. inject=600:START:6/700:DOWN:6. Lets menus be driven in tests without
 * sending key events to the emulator window. Buttons: START SELECT UP DOWN LEFT RIGHT CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2. */
static int s_noAudio;
void ps2_set_noaudio(int disabled) { s_noAudio = disabled ? 1 : 0; }
int ps2_noaudio(void) { return s_noAudio; }

static int s_eagerStream;
void ps2_set_eagerstream(int eager) { s_eagerStream = eager ? 1 : 0; }
int ps2_eagerstream(void) { return s_eagerStream; }

static int s_openVideo;
void ps2_set_open_video(int on) { s_openVideo = on ? 1 : 0; }
int ps2_open_video(void) { return s_openVideo; }
static char s_openMenu[32];
void ps2_set_open_menu(const char *tag)
{
    strncpy(s_openMenu, tag, sizeof(s_openMenu) - 1);
    s_openMenu[sizeof(s_openMenu) - 1] = 0;
}
/* The menu tag to open by itself ("Video WIN" for the openvideo flag), or NULL. */
const char *ps2_open_menu(void)
{
    if (s_openMenu[0])
        return s_openMenu;
    return s_openVideo ? "Video WIN" : NULL;
}

static int s_audioCheck;
void ps2_set_audiocheck(int on) { s_audioCheck = on ? 1 : 0; }
int ps2_audiocheck(void) { return s_audioCheck; }
static int s_achTest;
void ps2_set_achtest(int on) { s_achTest = on ? 1 : 0; }
int ps2_achtest(void) { return s_achTest; }
static int s_strStress;
void ps2_set_strstress(int on) { s_strStress = on ? 1 : 0; }
int ps2_strstress(void) { return s_strStress; }
static int s_lodPinned = -1;
void ps2_set_lod(int level) { s_lodPinned = level; }
int ps2_lod_pinned(void) { return s_lodPinned; }
static int s_lodCmp = -1;
void ps2_set_lodcmp(int level) { s_lodCmp = level; }
int ps2_lodcmp(void) { return s_lodCmp; }
static int s_bot;
void ps2_set_bot(int on) { s_bot = on ? 1 : 0; }

#define PS2_INJECT_MAX 96
static struct { uint32_t at, len; uint16_t mask; } s_inject[PS2_INJECT_MAX];
static int s_injectCount;
static uint32_t s_padPolls;

void ps2_inject_set(const char *spec)
{
    static const struct { const char *name; uint16_t mask; } names[] = {
        { "START", PS2_PAD_START }, { "SELECT", PS2_PAD_SELECT }, { "UP", PS2_PAD_UP }, { "DOWN", PS2_PAD_DOWN }, { "LEFT", PS2_PAD_LEFT },
        { "RIGHT", PS2_PAD_RIGHT }, { "CROSS", PS2_PAD_CROSS }, { "CIRCLE", PS2_PAD_CIRCLE }, { "SQUARE", PS2_PAD_SQUARE },
        { "TRIANGLE", PS2_PAD_TRIANGLE }, { "L1", PS2_PAD_L1 }, { "R1", PS2_PAD_R1 }, { "L2", PS2_PAD_L2 }, { "R2", PS2_PAD_R2 },
    };
    s_injectCount = 0;
    while (spec && *spec && *spec != ';' && s_injectCount < PS2_INJECT_MAX) {
        char name[16];
        unsigned at = 0, len = 0;
        int n = 0;
        if (sscanf(spec, "%u:%15[A-Za-z0-9]:%u%n", &at, name, &len, &n) < 3)
            break;
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            if (strcmp(name, names[i].name) == 0) {
                s_inject[s_injectCount].at = at;
                s_inject[s_injectCount].len = len;
                s_inject[s_injectCount].mask = names[i].mask;
                ++s_injectCount;
                break;
            }
        }
        spec += n;
        if (*spec == '/')
            ++spec;
    }
    ps2_log("[boot] %d scripted input events\n", s_injectCount);
}

void ps2_report_screen(void);
static uint64_t s_reportSince; /* time R1 + L2 + SELECT have been down together (0 = not all down) */
/* R1+L2+SELECT held for 0.4 s of real time (not a number of polls: a heavy scene polls the pad only 20 times a second): the report screen */
static unsigned s_reportAtSec; /* developer (boot.txt reportat=S): open the report screen from the first file load after S seconds, as if the buttons were held */
void ps2_set_reportat(int s) { s_reportAtSec = s > 0 ? (unsigned)s : 0; }
static void ps2_report_feed(uint16_t buttons)
{
    if (s_reportAtSec && ps2_time_us() > (uint64_t)s_reportAtSec * 1000000ull) {
        s_reportAtSec = 0;
        ps2_report_screen();
    }
    const uint16_t all = PS2_PAD_R1 | PS2_PAD_L2 | PS2_PAD_SELECT;
    if ((buttons & all) != all) {
        s_reportSince = 0;
        return;
    }
    const uint64_t now = ps2_time_us();
    if (!s_reportSince)
        s_reportSince = now;
    else if (now - s_reportSince >= 400000ull)
        ps2_report_screen();
}
/* Reads port 0 directly: called while a load is running and no game frames are polling the pad. */
void ps2_report_poll_pad(void)
{
    static uint64_t last;
    const uint64_t now = ps2_time_us();
    if (now - last < 60000ull || !s_padOpen[0])
        return;
    last = now;
    struct padButtonStatus b;
    if (padGetState(0, 0) != PAD_STATE_STABLE || padRead(0, 0, &b) == 0)
        return;
    ps2_report_feed((uint16_t)(0xFFFF ^ b.btns));
}
void ps2_pad_read(int port, ps2_pad_state *out)
{
    struct padButtonStatus b;

    memset(out, 0, sizeof(*out));
    out->lx = out->ly = out->rx = out->ry = 128;

    if (port == 0 && s_bot) {
        // Unattended play: run right, jump in bursts. Polls are engine frames.
        const uint32_t poll = s_padPolls++;
        out->connected = 1;
        if (poll > 300) {
            out->buttons |= PS2_PAD_RIGHT;
            if (((poll / 25) % 3) != 2) out->buttons |= PS2_PAD_CROSS;
            if (((poll / 400) % 5) == 4) { out->buttons |= PS2_PAD_DOWN; out->buttons &= (uint16_t)~PS2_PAD_RIGHT; }
        }
    }
    if (port == 0 && s_injectCount) {
        const uint32_t poll = s_padPolls++;
        if ((poll % 300) == 0)
            ps2_log("[inject] polls=%u\n", (unsigned)poll);
        for (int i = 0; i < s_injectCount; ++i) {
            if (poll == s_inject[i].at)
                ps2_log("[inject] poll %u: mask 0x%04x for %u frames\n", (unsigned)poll, (unsigned)s_inject[i].mask, (unsigned)s_inject[i].len);
            if (poll >= s_inject[i].at && poll < s_inject[i].at + s_inject[i].len) {
                out->connected = 1;
                out->buttons |= s_inject[i].mask;
            }
        }
    }

    if (port < 0 || port > 1 || !s_padOpen[port])
        return;
    if (padGetState(port, 0) != PAD_STATE_STABLE)
        return;
    ps2_pad_setup_mode(port);
    if (padRead(port, 0, &b) == 0)
        return;

    out->connected = 1;
    out->buttons |= (uint16_t)(0xFFFF ^ b.btns); /* hardware reports active-low */
    if (port == 0)
        ps2_report_feed(out->buttons);
    out->lx = b.ljoy_h;
    out->ly = b.ljoy_v;
    out->rx = b.rjoy_h;
    out->ry = b.rjoy_v;
}

/* ------------------------------------------------------------------------- */
/* Audio (audsrv)                                                            */
/* ------------------------------------------------------------------------- */

/* IOP modules linked into the ELF (irx_gen/*.c, bin2c arrays) are started with SifExecModuleBuffer. A console that boots from USB has no host:
 * to load them from, so the audio and USB modules all travel inside the ELF. The retail BIOS LOADFILE service ignores "load from buffer"
 * requests until sbv_patch_enable_lmb() has patched it. Without the patch the call returns the IOP buffer address as if it were a module id and
 * nothing is started, which is what left audsrv_init() and fileXioInit() waiting forever in the earlier attempts. */
extern unsigned char libsd_irx[], audsrv_irx[];
extern unsigned int size_libsd_irx, size_audsrv_irx;

static int s_lmbPatched;

static int ps2_iop_load_buffer(const char *name, void *irx, unsigned size)
{
    if (!s_lmbPatched) {
        s_lmbPatched = 1;
        SifInitRpc(0);
        const int lmb = sbv_patch_enable_lmb();
        ps2_log("[iop] sbv_patch_enable_lmb -> %d\n", lmb);
    }
    int ret = 0;
    const int id = SifExecModuleBuffer(irx, size, 0, NULL, &ret);
    ps2_log("[iop] %s: module id %d, return %d\n", name, id, ret);
    /* A real module id is small and positive. A huge value is the IOP buffer address coming back unchanged, i.e. the request was ignored. */
    return id > 0 && id < 0x1000;
}

/* The audio IOP modules (libsd.irx = freesd, audsrv.irx) come from the ELF. If that fails the game still starts, without sound. */
static int s_audioReady;

int ps2_audio_init(int rate)
{
    ps2_log("[audio] starting libsd / audsrv\n");
    if (!ps2_iop_load_buffer("libsd", libsd_irx, size_libsd_irx) || !ps2_iop_load_buffer("audsrv", audsrv_irx, size_audsrv_irx)) {
        ps2_log("[audio] IOP modules did not start: sound disabled\n");
        return 0;
    }
    if (audsrv_init() != 0) {
        ps2_log("[audio] audsrv_init failed: %s\n", audsrv_get_error_string());
        return 0;
    }

    struct audsrv_fmt_t fmt;
    fmt.bits = 16;
    fmt.freq = rate;
    fmt.channels = 2;
    if (audsrv_set_format(&fmt) != AUDSRV_ERR_NOERROR) {
        ps2_log("[audio] audsrv_set_format failed: %s\n", audsrv_get_error_string());
        return 0;
    }
    audsrv_set_volume(MAX_VOLUME);

    s_audioReady = 1;
    ps2_log("[audio] audsrv ready: %d Hz, 16-bit stereo\n", rate);
    return 1;
}
/* While the game blocks for longer than the audio ring lasts (about 100 ms: a scene load, a save), nothing refills the ring and the sound
 * hardware keeps replaying the last block it was given: a buzzing, stuttering repeat. Pausing stops the output (silence) and makes the ring
 * look full, so the mixer neither mixes nor queues; the music and effects stay where they were and carry on after ps2_audio_resume(). */
static int s_audioPaused;
static uint64_t s_audioPausedAt;

static int s_noPause;
void ps2_set_nopause(int on) { s_noPause = on; }

/* ---- mixer thread ----
 * The engine mixes the music (Vorbis decode) and the effects in software. Done from the frame loop, it starved whenever a frame ran longer than the 106 ms
 * sound ring (a console is slower than the emulator): the sound went away in every busy scene and worked only on the title screen. The mixer now
 * runs in a thread of its own that wakes every few milliseconds and tops the ring up, whatever the game thread is doing. LockAudioDevice() (which the
 * engine already calls around every change to channels and sounds, from when it ran an audio thread under SDL) is a recursive lock; audsrv calls and the
 * mixer step are made under it, so the scene load (which pauses the audio first) can free sounds safely. */
static int s_lockSema = -1, s_lockOwner = -1, s_lockCount;
static volatile int s_audioTid;
static void (*s_audioStep)(void);
static unsigned char s_audioStack[24 * 1024] __attribute__((aligned(16)));
extern void *_gp;

void ps2_audio_lock(void)
{
    if (s_lockSema < 0)
        return;
    const int me = GetThreadId();
    if (s_lockOwner == me) {
        ++s_lockCount;
        return;
    }
    WaitSema(s_lockSema);
    s_lockOwner = me;
    s_lockCount = 1;
}

void ps2_audio_unlock(void)
{
    if (s_lockSema < 0)
        return;
    if (s_lockOwner == GetThreadId() && --s_lockCount == 0) {
        s_lockOwner = -1;
        SignalSema(s_lockSema);
    }
}

int ps2_in_audio_thread(void) { return s_audioTid && GetThreadId() == s_audioTid; }
int ps2_audio_is_paused(void) { return s_audioPaused; }

static void ps2_audio_thread_main(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_audioPaused && s_audioStep)
            s_audioStep();
        usleep(5000);
    }
}

/* Starts the mixer thread; step() tops up the ring. Returns 1 if the thread runs (the frame loop must then not mix itself). */
int ps2_audio_thread_start(void (*step)(void))
{
    if (s_audioTid || !s_audioReady || s_noPause)
        return s_audioTid != 0;
    ee_sema_t sema;
    memset(&sema, 0, sizeof(sema));
    sema.init_count = 1;
    sema.max_count  = 1;
    s_lockSema      = CreateSema(&sema);
    if (s_lockSema < 0)
        return 0;
    ee_thread_status_t st;
    int mainPrio = 0;
    if (ReferThreadStatus(GetThreadId(), &st) == 0)
        mainPrio = st.current_priority;
    if (mainPrio < 10) /* the game thread gives way to the mixer: lower number = higher priority */
        ChangeThreadPriority(GetThreadId(), 10);
    ee_thread_t th;
    memset(&th, 0, sizeof(th));
    th.func             = (void *)ps2_audio_thread_main;
    th.stack            = s_audioStack;
    th.stack_size       = sizeof(s_audioStack);
    th.gp_reg           = &_gp;
    th.initial_priority = 5;
    const int id        = CreateThread(&th);
    if (id < 0) {
        ps2_log("[audio] CreateThread failed (%d): the frame loop mixes\n", id);
        return 0;
    }
    s_audioStep = step;
    s_audioTid  = id;
    if (StartThread(id, NULL) < 0) {
        s_audioTid = 0;
        ps2_log("[audio] StartThread failed: the frame loop mixes\n");
        return 0;
    }
    ps2_log("[audio] mixer thread %d started (game thread priority %d -> 10, mixer 5)\n", id, mainPrio);
    return 1;
}

/* Pauses nest: the scene load pauses, and the log flush or the report screen inside it pause and resume again; only the outermost resume
 * restarts the sound (an inner resume used to switch the mixer thread back on in the middle of a scene load, which hung the load). The sound is
 * muted with the volume, not with audsrv_stop_audio(): that call also freezes the ring until a later audsrv_play_audio(), which a full ring never gets
 * (the mixer writes only when there is room), and the sound stayed off for good. */
static int s_pauseDepth;
void ps2_audio_pause(void)
{
    if (!s_audioReady || s_noPause)
        return;
    ps2_audio_lock(); /* an audio slice that is being mixed finishes first */
    if (s_pauseDepth++ == 0) {
        audsrv_set_volume(0);
        s_audioPaused   = 1;
        s_audioPausedAt = ps2_time_us();
    }
    ps2_audio_unlock();
}

void ps2_audio_resume(void)
{
    if (!s_audioReady || s_pauseDepth <= 0)
        return;
    ps2_audio_lock();
    if (--s_pauseDepth == 0) {
        ps2_log("[audio] output was paused for %u ms (load or save)\n", (unsigned)((ps2_time_us() - s_audioPausedAt) / 1000));
        audsrv_set_volume(MAX_VOLUME);
        s_audioPaused = 0;
    }
    ps2_audio_unlock();
}

int ps2_audio_free_bytes(void)
{
    if (!s_audioReady || s_audioPaused)
        return 0;
    ps2_audio_lock();
    const int n = audsrv_available();
    ps2_audio_unlock();
    return n;
}

void ps2_audio_write(const void *pcm, int bytes)
{
    if (s_audioReady && !s_audioPaused) {
        ps2_audio_lock();
        audsrv_play_audio((const char *)pcm, bytes);
        ps2_audio_unlock();
    }
}
/* ------------------------------------------------------------------------- */
/* Save media (memory card)                                                  */
/* ------------------------------------------------------------------------- */

/* The user files are small (SaveData.bin is 64KB, the rest about 1KB). They live in a folder of their own on memory card slot 1.
 * libmc calls are asynchronous (they return at once and the result comes from mcSync), wrapped by mc_wait() below. */
#define PS2_MC_DIR   "SONICMANIAPS2"
#define PS2_MC_CHUNK 8192

/* mcOpen takes the PS2 file I/O open flags (fileio.h cannot be included in the newlib port: it #errors). Values per libmc-common.h
 * (sceMcFileCreateFile = 0x200 "equivalent to O_CREAT") and mcman (flags & 3 = read/write mode, 0x400 = truncate). */
#define PS2_MC_O_RDONLY 0x0001
#define PS2_MC_O_WRONLY 0x0002
#define PS2_MC_O_CREAT  0x0200
#define PS2_MC_O_TRUNC  0x0400

static int s_saveDevice = PS2_SAVE_HOST;
static int s_discBoot; /* 1 when started from a disc image: the boot volume is read-only, so user files go to the memory card or USB */
static int s_bootKnown;    /* the boot device has been decided (ps2_boot_dir_set): until then log lines are collected in case it turns out to be a disc */
static unsigned s_logLen;  /* bytes waiting in the disc-boot log buffer (see ps2_user_log_write) */
static int s_sio2Loaded;
static int s_mcState; /* 0 = not tried, 1 = ready, -1 = unavailable */
static int s_mcAllowFormat;
void ps2_save_allow_format(int allow) { s_mcAllowFormat = allow ? 1 : 0; }
static u8 *s_mcBuf;   /* 64-byte aligned staging buffer for the IOP DMA */

static void ps2_iop_sio2(void)
{
    if (s_sio2Loaded)
        return;
    s_sio2Loaded = 1;
    SifInitRpc(0);
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
}

static int s_saveDevicePinned; /* a boot.txt save=... flag wins over Settings.ini */

static const char *save_device_name(int device)
{
    return device == PS2_SAVE_MC ? "mc" : (device == PS2_SAVE_USB ? "usb" : "host");
}

void ps2_save_set_device(int device)
{
    s_saveDevice = device;
    ps2_log("[save] device: %s\n", device == PS2_SAVE_MC ? "memory card" : (device == PS2_SAVE_USB ? "USB" : "host"));
}

void ps2_save_pin_device(int device)
{
    s_saveDevicePinned = 1;
    ps2_save_set_device(device);
}

/* Settings.ini "saveDevice=host|mc|usb". Unknown text keeps the current device. Ignored when boot.txt pinned one. */
void ps2_save_set_device_by_name(const char *name)
{
    if (s_saveDevicePinned || !name)
        return;
    if (s_discBoot && strcmp(name, "host") == 0)
        return; /* the disc cannot be written to */
    if (strcmp(name, "mc") == 0)
        ps2_save_set_device(PS2_SAVE_MC);
    else if (strcmp(name, "usb") == 0)
        ps2_save_set_device(PS2_SAVE_USB);
    else if (strcmp(name, "host") == 0)
        ps2_save_set_device(PS2_SAVE_HOST);
    else
        ps2_log("[save] unknown saveDevice '%s' in Settings.ini, keeping current device\n", name);
}

const char *ps2_save_device_name(void) { return save_device_name(s_saveDevice); }

/* ---- USB mass storage ----
 * The IOP modules are linked into the ELF (irx_gen/*.c, made by tools/make_usb_image.sh with bin2c) because a console that boots from USB has no
 * host: to load them from. Order matters: iomanX (extended I/O manager) -> fileXio (its EE-side RPC server) -> usbd (USB stack) -> usbhdfsd
 * (FAT driver, registers the "mass:" device). With -lfileXio linked, plain fopen("mass:/...") reaches the USB stick. */
extern unsigned char iomanX_irx[], fileXio_irx[], usbd_irx[], usbhdfsd_irx[];
extern unsigned int size_iomanX_irx, size_fileXio_irx, size_usbd_irx, size_usbhdfsd_irx;

#define PS2_USB_DIR "mass:/SONICMANIAPS2"

static int s_usbModules; /* 1 once the four modules were started */
static int s_usbReady;   /* 1 once mass: answers */

static int usb_load(const char *name, void *irx, unsigned size) { return ps2_iop_load_buffer(name, irx, size); }

/* Loads the modules (once) and waits for a USB storage device to show up. The first call can take several seconds: the stick has to be
 * enumerated and its FAT volume mounted after the driver starts. Later calls only re-check. Returns 1 if mass: is usable. */
static int usb_prepare(void)
{
    if (s_usbReady)
        return 1;

    int waits = 8; /* later calls: ~0.8 s */
    if (!s_usbModules) {
        s_usbModules = 1;
        waits = 80; /* first call: up to ~8 s */
        if (!usb_load("iomanX", iomanX_irx, size_iomanX_irx) || !usb_load("fileXio", fileXio_irx, size_fileXio_irx)) {
            ps2_log("[usb] could not start the I/O manager modules\n");
            return 0;
        }
        const int fx = fileXioInit();
        ps2_log("[usb] fileXioInit -> %d\n", fx);
        if (!usb_load("usbd", usbd_irx, size_usbd_irx) || !usb_load("usbhdfsd", usbhdfsd_irx, size_usbhdfsd_irx)) {
            ps2_log("[usb] could not start the USB modules\n");
            return 0;
        }
    }

    for (int i = 0; i < waits; ++i) {
        DIR *d = opendir("mass:/");
        if (d) {
            closedir(d);
            mkdir(PS2_USB_DIR, 0777); /* fine if it already exists */
            s_usbReady = 1;
            ps2_log("[usb] mass: is ready after %d ms\n", i * 100);
            return 1;
        }
        usleep(100 * 1000);
    }
    ps2_log("[usb] no USB storage device found\n");
    return 0;
}

/* ---- Boot directory ---- */
static char s_bootDir[96] = "host:";

int ps2_boot_dir_is_usb(void) { return strncmp(s_bootDir, "mass", 4) == 0; }
const char *ps2_boot_dir(void) { return s_bootDir; }

int ps2_boot_dir_is_disc(void) { return s_discBoot; }

void ps2_boot_dir_set(const char *dir)
{
    if (!dir || !dir[0] || strlen(dir) >= sizeof(s_bootDir) - 1)
        return;
    strcpy(s_bootDir, dir);
    s_discBoot = strncmp(dir, "cdrom", 5) == 0;
    s_bootKnown = 1;
    if (!s_discBoot)
        s_logLen = 0; /* not a disc: the lines collected so far are not needed */
    ps2_log("[boot] boot directory: %s%s\n", s_bootDir, s_discBoot ? " (disc)" : "");
}

/* "Data.rsdk" -> "cdrom0:\DATA.RSDK;1". ISO 9660 level 1 names are upper case and carry a version number; the BIOS file system wants the full form. */
void ps2_disc_path(char *out, unsigned size, const char *name)
{
    unsigned n = (unsigned)snprintf(out, size, "cdrom0:\\");
    for (; *name && n + 3 < size; ++name) {
        char c = *name;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        else if (c == '/')
            c = '\\';
        out[n++] = c;
    }
    out[n++] = ';';
    out[n++] = '1';
    out[n]   = 0;
}

/* "mass:/GAMES/p2_engine.elf" -> "mass:/GAMES/", "host:p2_engine.elf" -> "host:", "mass0:/x.elf" -> "mass:/". Anything without a device keeps host:. */
void ps2_boot_dir_from_elf(const char *elfPath)
{
    if (!elfPath || !strchr(elfPath, ':'))
        return;
    if (strncmp(elfPath, "cdrom", 5) == 0) { /* "cdrom0:\SLUS_999.01;1" (or cdrom:) : started from a disc */
        ps2_boot_dir_set("cdrom0:\\");
        return;
    }
    char dir[96];
    if (strlen(elfPath) >= sizeof(dir))
        return;
    strcpy(dir, elfPath);
    char *colon = strchr(dir, ':');
    char *slash = strrchr(dir, '/');
    if (slash)
        slash[1] = 0;
    else
        colon[1] = 0;
    if (strncmp(dir, "mass", 4) == 0) { /* mass0:/ ... is the same stick: normalise the unit number away */
        char *c = strchr(dir, ':');
        memmove(dir + 4, c, strlen(c) + 1);
    }
    ps2_boot_dir_set(dir);
}

/* Whether a device can actually store saves right now. The options menu only offers devices for which this is true, so nobody can pick one that
 * would silently drop their progress. The memory card is reported available without touching it (it is checked when first used and a missing or
 * unformatted card is logged then). For USB this starts the USB stack on first use, which can take a few seconds. */
int ps2_save_device_available(int device)
{
    switch (device) {
        case PS2_SAVE_HOST: return !s_discBoot;
        case PS2_SAVE_MC: return 1;
        case PS2_SAVE_USB: return usb_prepare();
        default: return 0;
    }
}

/* The engine loads Achievements.bin before it reads Settings.ini, so the save device has to be known earlier: scan the file for the key here
 * (called once from main(), before the engine starts). A missing file or key leaves the device unchanged. */
void ps2_save_peek_settings(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "saveDevice=", 11) == 0) {
            char value[16] = { 0 };
            sscanf(line + 11, "%15s", value);
            ps2_save_set_device_by_name(value);
            break;
        }
    }
    fclose(f);
}

int ps2_save_device(void) { return s_saveDevice; }

static int mc_wait(void)
{
    int result = 0;
    mcSync(MC_WAIT, NULL, &result);
    return result;
}

static int mc_prepare(void)
{
    if (s_mcState)
        return s_mcState > 0;
    s_mcState = -1;

    ps2_iop_sio2();
    int mcman = SifLoadModule("rom0:XMCMAN", 0, NULL);
    int mcserv = SifLoadModule("rom0:XMCSERV", 0, NULL);
    int xmc = 1;
    if (mcman < 0 || mcserv < 0) {
        xmc = 0;
        mcman = SifLoadModule("rom0:MCMAN", 0, NULL);
        mcserv = SifLoadModule("rom0:MCSERV", 0, NULL);
    }
    if (mcman < 0 || mcserv < 0) {
        ps2_log("[save] memory card modules failed to load (mcman=%d mcserv=%d)\n", mcman, mcserv);
        return 0;
    }
    if (mcInit(xmc ? MC_TYPE_XMC : MC_TYPE_MC) < 0) {
        ps2_log("[save] mcInit failed\n");
        return 0;
    }

    /* The first mcGetInfo after init reports "card changed"; the second gives the real state. */
    int type = 0, freeKB = 0, format = 0, r = 0;
    mcGetInfo(0, 0, &type, &freeKB, &format);
    r = mc_wait();
    mcGetInfo(0, 0, &type, &freeKB, &format);
    r = mc_wait();
    if (r < -2 || type != MC_TYPE_PS2) {
        ps2_log("[save] no PS2 memory card in slot 1 (result %d, type %d)\n", r, type);
        return 0;
    }
    if (r == -2 || format != MC_FORMATTED) {
        if (!s_mcAllowFormat) {
            /* Never format a card automatically: it would wipe a real user's saves. Formatting is done from the PS2 browser. */
            ps2_log("[save] memory card in slot 1 is not formatted (format it from the PS2 BIOS browser)\n");
            return 0;
        }
        ps2_log("[save] developer flag mcformat: formatting the memory card in slot 1\n");
        mcFormat(0, 0);
        ps2_log("[save] mcFormat result %d\n", mc_wait());
        mcGetInfo(0, 0, &type, &freeKB, &format);
        r = mc_wait();
        if (r < -1 || format != MC_FORMATTED) {
            ps2_log("[save] card still not usable after formatting (result %d)\n", r);
            return 0;
        }
    }

    mcMkDir(0, 0, PS2_MC_DIR); /* fails harmlessly if it already exists */
    mc_wait();

    s_mcBuf = (u8 *)memalign(64, PS2_MC_CHUNK);
    if (!s_mcBuf) {
        ps2_log("[save] no memory for the card staging buffer\n");
        return 0;
    }
    s_mcState = 1;
    ps2_log("[save] memory card ready: %d KB free, %s modules\n", freeKB, xmc ? "X" : "standard");
    return 1;
}

/* ---- User directory for a disc boot ----
 * The disc is read-only, so Settings.ini and log.txt are plain files on the same device as the saves: "mc0:/SONICMANIAPS2/" (the memory card
 * modules are started by mc_prepare, which also creates the folder), or "mass:/SONICMANIAPS2/" when there is no card but a USB stick. */
static char s_userDir[40];
static int s_userDirReady; /* ps2_disc_user_dir_prepare() has run: until then log lines wait in the buffer */

void ps2_disc_user_dir_prepare(void)
{
    if (!s_discBoot)
        return;
    if (mc_prepare()) {
        snprintf(s_userDir, sizeof(s_userDir), "mc0:/" PS2_MC_DIR "/");
        s_saveDevice = PS2_SAVE_MC;
    }
    else if (usb_prepare()) {
        snprintf(s_userDir, sizeof(s_userDir), PS2_USB_DIR "/");
        s_saveDevice = PS2_SAVE_USB;
    }
    else {
        s_userDir[0] = 0;
        s_saveDevice = PS2_SAVE_MC;
        ps2_log("[boot] disc boot: no memory card and no USB storage, settings and saves cannot be kept\n");
    }
    s_userDirReady = 1;
    ps2_log("[boot] disc boot: user files in '%s' (save device %s)\n", s_userDir, save_device_name(s_saveDevice));
}

const char *ps2_user_dir(void)
{
    if (s_discBoot)
        return s_userDir;
    return ps2_boot_dir_is_usb() ? s_bootDir : "";
}

/* log.txt for a disc boot or a boot from USB. Writing to a memory card or a USB stick is slow on a real console (tens to hundreds of milliseconds per
 * open/write/close, and the memory card driver walks the whole file to find its end), so it is kept off the frame loop:
 *   - lines are collected in a RAM buffer; developer chatter (sprite sheet tables, per-frame profiling, ...) is not kept at all;
 *   - the buffer is written when it is full, and at the end of a scene load (ps2_log_checkpoint) if the last write is 15 s old, while the loading
 *     screen hides the delay;
 *   - at most PS2_LOG_FILE_CAP bytes are written this way; after that only the newest lines are kept in RAM, for the crash screen.
 * The first version wrote the file once a second for the first minute: on a real memory card that made the Main Menu take minutes to load. */
#define PS2_LOG_FILE_CAP 32768u
static char s_logBuf[3072];
static uint64_t s_logLast, s_logFirst;
static unsigned s_logFileBytes;
static int s_logStarted;
static int s_logBusy; /* flushing: lines logged meanwhile (audio pause/resume) are dropped */
static uint64_t s_logUs; /* time spent writing log.txt, for the report screen */
static unsigned s_logWrites;

static const char *log_dir(void) { return s_discBoot ? s_userDir : ps2_user_dir(); }
static int log_dir_ready(void) { return s_discBoot ? s_userDirReady : 1; }

static int log_is_chatter(const char *text)
{
    static const char *const noisy[] = { "[scenelist]", "[stg-alloc]", "[sheet]", "[io]", "[storage]", "[heap]", "[mem]", "[prof]", "[slow]", "[inject]",
                                         "Loaded data file", "[scene] entities", NULL };
    for (int i = 0; noisy[i]; ++i) {
        if (strncmp(text, noisy[i], strlen(noisy[i])) == 0)
            return 1;
    }
    return 0;
}

static void ps2_user_log_flush(int force)
{
    if (!s_logLen || !log_dir_ready() || s_logBusy)
        return;
    if (!force && s_logFileBytes + s_logLen > PS2_LOG_FILE_CAP) { /* the file is full: keep the newest lines for the crash screen */
        const unsigned keep = sizeof(s_logBuf) / 2;
        if (s_logLen > keep) {
            memmove(s_logBuf, s_logBuf + s_logLen - keep, keep);
            s_logLen = keep;
        }
        return;
    }
    s_logBusy = 1;
    const char *dir = log_dir();
    if (dir[0]) {
        char path[96];
        snprintf(path, sizeof(path), "%slog.txt", dir);
        const uint64_t tFlush = ps2_time_us();
        ps2_audio_pause();
        /* The memory card file driver ignores the append flag (every write would start at the beginning of the file), so a later flush opens the
         * file for update and seeks to its end itself. */
        FILE *f = fopen(path, s_logStarted ? "r+b" : "wb");
        if (f && s_logStarted)
            fseek(f, 0, SEEK_END);
        if (f) {
            fwrite(s_logBuf, 1, s_logLen, f);
            fclose(f);
        }
        ps2_audio_resume();
        s_logUs += ps2_time_us() - tFlush;
        ++s_logWrites;
    }
    s_logStarted = 1;
    s_logFileBytes += s_logLen;
    s_logLen  = 0;
    s_logLast = ps2_time_us();
    s_logBusy = 0;
}

void ps2_user_log_flush_now(void) { ps2_user_log_flush(1); }

/* called at the end of a scene load, behind the loading screen */
void ps2_log_checkpoint(void)
{
    if (!s_logLen || !s_bootKnown || (!s_discBoot && !ps2_boot_dir_is_usb()))
        return;
    if (ps2_time_us() - s_logLast >= 15000000ull)
        ps2_user_log_flush(0);
}

int ps2_user_log_write(const char *text)
{
    if (!s_discBoot && !ps2_boot_dir_is_usb() && s_bootKnown)
        return 0;
    if (s_logBusy)
        return 1;
    if (!s_bootKnown) { /* still in static initialisation: just keep the text (no clock, no file) */
        unsigned m = (unsigned)strlen(text);
        if (m > sizeof(s_logBuf) - 1 - s_logLen)
            m = sizeof(s_logBuf) - 1 - s_logLen;
        memcpy(s_logBuf + s_logLen, text, m);
        s_logLen += m;
        return 1;
    }
    if (log_is_chatter(text))
        return 1;
    const uint64_t now = ps2_time_us();
    if (!s_logFirst)
        s_logFirst = s_logLast = now;
    unsigned n = (unsigned)strlen(text);
    if (n > sizeof(s_logBuf) - 1)
        n = sizeof(s_logBuf) - 1;
    if (s_logLen + n > sizeof(s_logBuf))
        ps2_user_log_flush(0);
    if (s_logLen + n > sizeof(s_logBuf)) {
        if (!log_dir_ready())
            return 1; /* before the user directory exists the buffer keeps the earliest lines */
        s_logLen = 0; /* the file is full and the buffer too: the oldest lines go */
    }
    memcpy(s_logBuf + s_logLen, text, n);
    s_logLen += n;
    return 1;
}
static int ps2_save_write_impl(const char *name, const void *buffer, unsigned size)
{
    ps2_stage("save: writing a user file");
    if (s_saveDevice == PS2_SAVE_USB) {
        if (!usb_prepare())
            return -1;
        char upath[128];
        snprintf(upath, sizeof(upath), PS2_USB_DIR "/%s", name);
        FILE *uf = fopen(upath, "wb");
        if (!uf) {
            ps2_log("[save] cannot open %s for writing\n", upath);
            return -1;
        }
        const size_t wrote = fwrite(buffer, 1, size, uf);
        const int closed   = fclose(uf);
        if (wrote != size || closed != 0) {
            ps2_log("[save] short write to %s: %u of %u (close %d)\n", upath, (unsigned)wrote, size, closed);
            return -1;
        }
        return 0;
    }
    if (s_saveDevice != PS2_SAVE_MC || !mc_prepare())
        return -1;

    char path[96];
    snprintf(path, sizeof(path), "/" PS2_MC_DIR "/%s", name);

    mcOpen(0, 0, path, PS2_MC_O_WRONLY | PS2_MC_O_CREAT | PS2_MC_O_TRUNC);
    const int fd = mc_wait();
    if (fd < 0) {
        ps2_log("[save] cannot open %s for writing (%d)\n", path, fd);
        return -1;
    }

    const u8 *src = (const u8 *)buffer;
    unsigned done = 0;
    int ok = 1;
    while (done < size) {
        unsigned n = size - done;
        if (n > PS2_MC_CHUNK)
            n = PS2_MC_CHUNK;
        memcpy(s_mcBuf, src + done, n);
        SyncDCache(s_mcBuf, s_mcBuf + n);
        mcWrite(fd, s_mcBuf, (int)n);
        const int w = mc_wait();
        if (w != (int)n) {
            ps2_log("[save] short write to %s: %d of %u\n", path, w, n);
            ok = 0;
            break;
        }
        done += n;
    }
    mcClose(fd);
    mc_wait();
    return ok ? 0 : -1;
}

/* A memory card or USB write blocks for longer than the audio ring lasts, so the sound is paused around it (see ps2_audio_pause). */
int ps2_save_write(const char *name, const void *buffer, unsigned size)
{
    ps2_audio_pause();
    const int r = ps2_save_write_impl(name, buffer, size);
    ps2_audio_resume();
    return r;
}
int ps2_save_read(const char *name, void *buffer, unsigned size)
{
    ps2_stage("save: reading a user file");
    if (s_saveDevice == PS2_SAVE_USB) {
        if (!usb_prepare())
            return -1;
        char upath[128];
        snprintf(upath, sizeof(upath), PS2_USB_DIR "/%s", name);
        FILE *uf = fopen(upath, "rb");
        if (!uf)
            return -1; /* does not exist yet: normal on first run */
        const size_t got = fread(buffer, 1, size, uf);
        fclose(uf);
        return (int)got;
    }
    if (s_saveDevice != PS2_SAVE_MC || !mc_prepare())
        return -1;

    char path[96];
    snprintf(path, sizeof(path), "/" PS2_MC_DIR "/%s", name);

    mcOpen(0, 0, path, PS2_MC_O_RDONLY);
    const int fd = mc_wait();
    if (fd < 0)
        return -1; /* does not exist yet: normal on first run */

    mcSeek(fd, 0, SEEK_END);
    const int fileSize = mc_wait();
    mcSeek(fd, 0, SEEK_SET);
    mc_wait();

    unsigned want = (fileSize >= 0 && (unsigned)fileSize < size) ? (unsigned)fileSize : size;
    u8 *dst = (u8 *)buffer;
    unsigned done = 0;
    while (done < want) {
        unsigned n = want - done;
        if (n > PS2_MC_CHUNK)
            n = PS2_MC_CHUNK;
        SyncDCache(s_mcBuf, s_mcBuf + n);
        mcRead(fd, s_mcBuf, (int)n);
        const int r = mc_wait();
        if (r <= 0)
            break;
        InvalidDCache(s_mcBuf, s_mcBuf + r);
        memcpy(dst + done, s_mcBuf, (unsigned)r);
        done += (unsigned)r;
    }
    mcClose(fd);
    mc_wait();
    return (int)done;
}

int ps2_save_delete(const char *name)
{
    if (s_saveDevice == PS2_SAVE_USB) {
        if (!usb_prepare())
            return -1;
        char upath[128];
        snprintf(upath, sizeof(upath), PS2_USB_DIR "/%s", name);
        return remove(upath) == 0 ? 0 : -1;
    }
    if (s_saveDevice != PS2_SAVE_MC || !mc_prepare())
        return -1;
    char path[96];
    snprintf(path, sizeof(path), "/" PS2_MC_DIR "/%s", name);
    mcDelete(0, 0, path);
    return mc_wait() == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* Data streaming, preload and the loading indicator                          */
/* ------------------------------------------------------------------------- */

/* Settings > Video > DATA STREAM: ON = assets and music are read from the medium while playing (the original behaviour); OFF = everything that can be
 * is loaded before the level is shown (music tracks are read into memory, see Audio.cpp) and a LOADING indicator is shown during loads; AUTO = OFF when
 * the medium is slow (started from USB, or a measured read speed under 2.5 MB/s), else ON. USB 1.1 reads about 0.5 MB/s: a 16 KB music refill can stall
 * a frame, and loading a track in the middle of a level used to freeze the game for a second or two. */
static int s_streamMode; /* 0 = AUTO, 1 = ON, 2 = OFF */
static int s_mediaSlow = -1;
static int s_mediaKBs;
static int s_readDelayMs; /* developer (boot.txt readdelay=MS): sleep this long on every 32 KB read during a load, to imitate a slow medium */

int ps2_stream_mode(void) { return s_streamMode; }
static int s_streamPinned; /* boot.txt stream=on|off wins over Settings.ini */
void ps2_set_stream_mode(int m) { s_streamMode = (m >= 0 && m <= 2) ? m : 0; }
void ps2_pin_stream_mode(int m)
{
    ps2_set_stream_mode(m);
    s_streamPinned = 1;
}
const char *ps2_stream_mode_name(void) { return s_streamMode == 1 ? "on" : (s_streamMode == 2 ? "off" : "auto"); }
void ps2_set_stream_mode_by_name(const char *name)
{
    if (!name || s_streamPinned)
        return;
    if (strcmp(name, "on") == 0)
        s_streamMode = 1;
    else if (strcmp(name, "off") == 0)
        s_streamMode = 2;
    else
        s_streamMode = 0;
}
void ps2_set_readdelay(int ms) { s_readDelayMs = ms > 0 ? ms : 0; }

/* Reads 256 KB (8 x 32 KB) from the middle of Data.rsdk and times it. Started from USB the answer is "slow" without reading. */
static void ps2_media_probe(void)
{
    s_mediaSlow = 0;
    if (ps2_boot_dir_is_usb()) {
        s_mediaSlow = 1;
        ps2_log("[boot] started from USB: slow medium\n");
        return;
    }
    char path[96];
    if (s_discBoot)
        ps2_disc_path(path, sizeof(path), "Data.rsdk");
    else
        snprintf(path, sizeof(path), "%sData.rsdk", ps2_user_dir());
    FILE *f = fopen(path, "rb");
    char *buf = (char *)malloc(32768);
    if (!f || !buf) {
        if (f)
            fclose(f);
        free(buf);
        ps2_log("[boot] media speed: could not probe %s\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    long at = size > (96l << 20) ? (64l << 20) : 0;
    fseek(f, at, SEEK_SET);
    const uint64_t t0 = ps2_time_us();
    size_t total = 0;
    for (int i = 0; i < 8; ++i) total += fread(buf, 1, 32768, f);
    const uint64_t us = ps2_time_us() - t0;
    fclose(f);
    free(buf);
    s_mediaKBs = us ? (int)((uint64_t)(total / 1024) * 1000000ull / us) : 99999;
    s_mediaSlow = s_mediaKBs < 2500;
    ps2_log("[boot] media speed: %d KB/s (%u KB in %u ms) -> %s\n", s_mediaKBs, (unsigned)(total / 1024), (unsigned)(us / 1000), s_mediaSlow ? "slow" : "fast");
}

/* 1 when assets should be loaded ahead of the level (preload mode) and the loading indicator shown */
int ps2_preload_active(void)
{
    if (s_streamMode == 1)
        return 0;
    if (s_streamMode == 2)
        return 1;
    if (s_mediaSlow < 0)
        ps2_media_probe();
    return s_mediaSlow;
}

/* ---- loading indicator ----
 * phase 0 = a load begins (the hook saves the picture under the indicator), 1 = draw the next animation frame and show it, 2 = the load ended.
 * ps2_loading_tick() is called from the places that spend the time (file reads in 32 KB steps, after every asset) and shows a frame about every 90 ms. */
static void (*s_loadHook)(int phase);
static int s_loadActive;
static uint32_t s_loadLast;
void ps2_set_loading_hook(void (*fn)(int phase)) { s_loadHook = fn; }
int ps2_loading_active(void) { return s_loadActive; }

void ps2_loading_begin(void)
{
    if (ps2_in_audio_thread())
        return;
    if (s_loadActive || !s_loadHook || !ps2_preload_active())
        return;
    s_loadActive = 1;
    s_loadLast   = ps2_cycles();
    s_loadHook(0);
    s_loadHook(1);
}

void ps2_loading_tick(void)
{
    if (ps2_in_audio_thread())
        return; /* the mixer thread reopens the music file through LoadFile: it must not poll the pad or draw the loading screen */
    ps2_report_poll_pad(); /* a load can take minutes without a single game frame: the report screen must still open */
    if (!s_loadActive)
        return;
    const uint32_t now = ps2_cycles();
    if ((now - s_loadLast) / PS2_CYCLES_PER_US < 90000u)
        return;
    s_loadLast = now;
    s_loadHook(1);
}

void ps2_loading_end(void)
{
    if (!s_loadActive)
        return;
    s_loadActive = 0;
    s_loadHook(2);
}

/* Every read the C library makes on a file goes through __wrap__read (linker flag -Wl,--wrap=_read). It counts calls and bytes, and with the developer
 * flag usbsim=MS it costs MS milliseconds per call plus 1 ms per KB, which imitates a USB 1.1 stick (each call is a request to the IOP and the drive). */
extern int __real__read(int fd, void *buf, size_t n);
static int s_simCallMs;
static unsigned s_ioCalls, s_ioBytes, s_ioLastCalls, s_ioLastBytes;
static int s_ioVerbose;
void ps2_set_usbsim(int ms)
{
    s_simCallMs = ms > 0 ? ms : 0;
    s_ioVerbose = ms > 0;
}
static uint64_t s_ioUs; /* time spent inside library reads (all files), for the report screen */
int __wrap__read(int fd, void *buf, size_t n)
{
    const uint64_t t0 = ps2_time_us();
    if (fd > 2) {
        ++s_ioCalls;
        if (s_simCallMs)
            usleep((useconds_t)(s_simCallMs * 1000u + (n >> 10) * 1000u));
    }
    const int r = __real__read(fd, buf, n);
    if (fd > 2) {
        if (r > 0)
            s_ioBytes += (unsigned)r;
        s_ioUs += ps2_time_us() - t0;
    }
    return r;
}
/* log the reads since the last call: how many requests the library made and how many bytes they returned */
void ps2_io_log(const char *what)
{
    if (!s_ioVerbose)
        return;
    ps2_log("[io] %s: %u reads, %u bytes\n", what, s_ioCalls - s_ioLastCalls, s_ioBytes - s_ioLastBytes);
    s_ioLastCalls = s_ioCalls;
    s_ioLastBytes = s_ioBytes;
}
/* ---- report screen ----
 * Hold R1 + L2 + SELECT for half a second (while the game polls the pad, so not during a load) and the game stops on a text screen with the time
 * each scene load took, how much of it went into reading files and into writing log.txt, and the last log lines. It is for people who cannot read
 * files from the memory card: photograph it. The console has to be restarted afterwards. */
#define PS2_MARKS 12
static struct {
    char name[12];
    uint32_t loadMs, reads, kb, ioMs, logMs;
} s_marks[PS2_MARKS];
static int s_markCount;
static uint64_t s_mbT, s_mbIo, s_mbLog;
static unsigned s_mbReads, s_mbBytes;

static unsigned s_mixWakes, s_mixSlices, s_mixAvgUs, s_mixEmpty;
static int s_mixMaxFree;
void ps2_audio_note_mix(unsigned wakes, unsigned slices, unsigned avgUs, int maxFree, unsigned empty)
{
    s_mixWakes   = wakes;
    s_mixSlices  = slices;
    s_mixAvgUs   = avgUs;
    s_mixMaxFree = maxFree;
    s_mixEmpty   = empty;
}
static unsigned s_loopSeekWorstUs, s_loopSeekCount, s_loopSeekLastUs, s_loopSeekWorstReads;
void ps2_audio_note_loopseek(unsigned us, unsigned reads)
{
    ++s_loopSeekCount;
    s_loopSeekLastUs = us;
    if (us > s_loopSeekWorstUs) {
        s_loopSeekWorstUs    = us;
        s_loopSeekWorstReads = reads;
    }
    ps2_log("[audio] music loop seek: %u.%u ms, %u file reads (loops so far %u, worst %u.%u ms)\n", us / 1000u, (us / 100u) % 10u, reads, s_loopSeekCount,
            s_loopSeekWorstUs / 1000u, (s_loopSeekWorstUs / 100u) % 10u);
}

static int s_bonusTest;
void ps2_set_bonustest(int on) { s_bonusTest = on; }
int ps2_bonus_test_due(void)
{
    static uint64_t first;
    if (!s_bonusTest)
        return 0;
    if (!first)
        first = ps2_time_us();
    if (ps2_time_us() - first < 20000000ull)
        return 0;
    const int which = s_bonusTest;
    s_bonusTest     = 0;
    return which;
}

static char s_lastFile[56];
static uint64_t s_lastFileAt;
void ps2_note_file(const char *name)
{
    const size_t n = name ? strlen(name) : 0;
    snprintf(s_lastFile, sizeof(s_lastFile), "%s", n > sizeof(s_lastFile) - 1 ? name + n - (sizeof(s_lastFile) - 1) : (name ? name : ""));
    s_lastFileAt = ps2_time_us();
}

void ps2_load_begin_mark(void)
{
    s_mbT     = ps2_time_us();
    s_mbReads = s_ioCalls;
    s_mbBytes = s_ioBytes;
    s_mbIo    = s_ioUs;
    s_mbLog   = s_logUs;
}

void ps2_load_end_mark(const char *name)
{
    int i = s_markCount % PS2_MARKS;
    snprintf(s_marks[i].name, sizeof(s_marks[i].name), "%s", name ? name : "?");
    s_marks[i].loadMs = (uint32_t)((ps2_time_us() - s_mbT) / 1000u);
    s_marks[i].reads  = s_ioCalls - s_mbReads;
    s_marks[i].kb     = (s_ioBytes - s_mbBytes) / 1024u;
    s_marks[i].ioMs   = (uint32_t)((s_ioUs - s_mbIo) / 1000u);
    s_marks[i].logMs  = (uint32_t)((s_logUs - s_mbLog) / 1000u);
    ++s_markCount;
}

void ps2_report_screen(void)
{
    init_scr();
    scr_setbgcolor(0x600000); /* blue */
    scr_setfontcolor(0xFFFFFF);
    scr_clear();
    DIAG_MAGNIFY_MID();
    scr_setXY(0, 3);
    char t[160];
    scr_printf(" SONIC MANIA PS2 REPORT   built %s %s\n", __DATE__, __TIME__);
    snprintf(t, sizeof(t), "stream %s  media %d KB/s %s", ps2_stream_mode_name(), s_mediaKBs,
             ps2_boot_dir_is_disc() ? "disc" : (ps2_boot_dir_is_usb() ? "usb" : "host"));
    diag_put_w(t, PS2_CON_WRAP);
    snprintf(t, sizeof(t), "reads %u / %u MB in %u.%u s;  log writes %u in %u ms", s_ioCalls, s_ioBytes >> 20, (unsigned)(s_ioUs / 1000000ull),
             (unsigned)((s_ioUs / 100000ull) % 10u), s_logWrites, (unsigned)(s_logUs / 1000u));
    diag_put_w(t, PS2_CON_WRAP);
    snprintf(t, sizeof(t), "last file %.40s, %u.%u s ago", s_lastFile, (unsigned)((ps2_time_us() - s_lastFileAt) / 1000000ull),
             (unsigned)(((ps2_time_us() - s_lastFileAt) / 100000ull) % 10u));
    diag_put_w(t, PS2_CON_WRAP);
    snprintf(t, sizeof(t), "music loops %u worst %u.%u ms; mixer: %u slices %u us, ring starved %u of %u, free max %d", s_loopSeekCount,
             s_loopSeekWorstUs / 1000u, (s_loopSeekWorstUs / 100u) % 10u, s_mixSlices, s_mixAvgUs, s_mixEmpty, s_mixWakes, s_mixMaxFree);
    diag_put_w(t, PS2_CON_WRAP);
    snprintf(t, sizeof(t), "stage: %s", s_stageText);
    diag_put_w(t, PS2_CON_WRAP);
    scr_printf(" scene loads (name, total, in reads, KB, reads, log):\n");
    const int first = s_markCount > 3 ? s_markCount - 3 : 0;
    for (int k = first; k < s_markCount; ++k) {
        const int i = k % PS2_MARKS;
        snprintf(t, sizeof(t), "%-8.8s %u.%us io %u.%us %uKB %ur log %ums", s_marks[i].name, (unsigned)(s_marks[i].loadMs / 1000u),
                 (unsigned)((s_marks[i].loadMs / 100u) % 10u), (unsigned)(s_marks[i].ioMs / 1000u), (unsigned)((s_marks[i].ioMs / 100u) % 10u),
                 (unsigned)s_marks[i].kb, (unsigned)s_marks[i].reads, (unsigned)s_marks[i].logMs);
        diag_put_w(t, PS2_CON_WRAP);
    }
    int lines = 0; /* (no room for log lines) */
    for (int k = s_diagCount - lines; k < s_diagCount; ++k) scr_printf(" %.46s\n", s_diagLine[k % PS2_DIAG_ROWS]);
    scr_printf(" Please photograph this, then restart the console.\n");
    ps2_user_log_flush_now();
    for (;;) {
    }
}
/* fread in 32 KB steps while the loading indicator is up, so that it keeps moving during a long read. With the developer flag readdelay=MS every 32 KB
 * read costs MS milliseconds (counted over all reads, so many small reads add up the same as one big one). */
size_t ps2_read_ticked(void *dst, size_t n, void *file)
{
    static size_t delayBytes;
    size_t done = 0;
    char *p     = (char *)dst;
    if (!s_loadActive) {
        done = fread(p, 1, n, (FILE *)file);
    }
    else {
        while (done < n) {
            const size_t step = (n - done) > 32768 ? 32768 : (n - done);
            const size_t got  = fread(p + done, 1, step, (FILE *)file);
            done += got;
            if (got == step)
                ps2_loading_tick();
            if (got < step)
                break;
        }
    }
    if (s_readDelayMs) {
        delayBytes += done;
        while (delayBytes >= 32768) {
            delayBytes -= 32768;
            usleep((useconds_t)s_readDelayMs * 1000u);
            ps2_loading_tick();
        }
    }
    return done;
}
/* Writes a 64KB pattern, reads it back and compares (boot.txt "savetest"). Run before the engine allocates its pools so the buffers fit. */
void ps2_save_selftest(void)
{
    const unsigned size = 65536;
    u8 *a = (u8 *)malloc(size);
    u8 *b = (u8 *)malloc(size);
    if (!a || !b) {
        ps2_log("[save] selftest: no memory\n");
        free(a);
        free(b);
        return;
    }
    for (unsigned i = 0; i < size; ++i) a[i] = (u8)(i * 31u + (i >> 8) + 7u);
    memset(b, 0, size);

    const int w = ps2_save_write("selftest.bin", a, size);
    const int r = ps2_save_read("selftest.bin", b, size);
    unsigned bad = 0;
    for (unsigned i = 0; i < size; ++i) bad += a[i] != b[i];
    ps2_log("[save] selftest: write=%d read=%d bytes, %u mismatching bytes -> %s\n", w, r, bad, (w == 0 && r == (int)size && bad == 0) ? "PASS" : "FAIL");

    /* second write with a shorter file must truncate */
    const int w2 = ps2_save_write("selftest.bin", a, 1000);
    const int r2 = ps2_save_read("selftest.bin", b, size);
    ps2_log("[save] selftest: rewrite 1000 bytes -> read back %d bytes (%s)\n", r2, (w2 == 0 && r2 == 1000) ? "PASS" : "FAIL");

    ps2_log("[save] selftest: delete -> %d\n", ps2_save_delete("selftest.bin"));
    free(a);
    free(b);
}

/* ------------------------------------------------------------------------- */
/* Memory diagnostics                                                        */
/* ------------------------------------------------------------------------- */

void ps2_heap_report(const char *label)
{
    extern char _end[];
    struct mallinfo mi = mallinfo();
    ps2_log("[heap] %s: arena=%u used=%u free=%u sbrk=%p _end=%p\n", label, (unsigned)mi.arena, (unsigned)mi.uordblks, (unsigned)mi.fordblks,
            sbrk(0), (void *)_end);
}

uint32_t ps2_heap_used(void)
{
    struct mallinfo mi = mallinfo();
    return (uint32_t)mi.uordblks;
}

uint32_t ps2_ram_total(void)
{
    return 32u * 1024u * 1024u;
}

