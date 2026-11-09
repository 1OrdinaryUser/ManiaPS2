/*
 * PS2 render device.
 *
 * The engine software-renders into screens[n].frameBuffer (RGB565). This backend only presents
 * screen 0: it hands the buffer to the HAL, which converts it to GS format, uploads it as a texture
 * and draws one stretched quad. Split-screen, dimming and video/image textures are not handled yet.
 */
#include "RSDK/PS2/ps2_hal.h"

// Render widths for the two Aspect ratio settings. Both must be <= SCREEN_XMAX and <= the 512-pixel upload texture.
#define PS2_WIDE_PIXWIDTH   424 // 16:9
#define PS2_NARROW_PIXWIDTH 320 // 4:3

// ---- Loading indicator: Sonic running (frames read from the game data, see PS2LoadingAnimInit) and "LOADING..." in the bottom-right corner ----
// Drawn straight into the frame buffer of screen 0, which still holds the last picture while a scene loads, and shown with ps2_gfx_present(). The picture
// under the indicator is kept in the last screen's buffer (unused in single player) and put back before every frame, so the animation does not smear.
// Only shown in preload mode (Settings > Video > DATA STREAM OFF, or AUTO on a slow medium, see ps2_hal.h).
#define PS2_LOAD_W 94
#define PS2_LOAD_H 22

static const uint8 ps2LoadGlyphs[8][7] = { { 16, 16, 16, 16, 16, 16, 31 },  // L
                                           { 14, 17, 17, 17, 17, 17, 14 },  // O
                                           { 14, 17, 17, 31, 17, 17, 17 },  // A
                                           { 30, 17, 17, 17, 17, 17, 30 },  // D
                                           { 14, 4, 4, 4, 4, 4, 14 },       // I
                                           { 17, 25, 21, 19, 17, 17, 17 },  // N
                                           { 14, 17, 16, 23, 17, 17, 15 },  // G
                                           { 0, 0, 0, 0, 0, 0, 4 } };       // .
static const uint8 ps2LoadWord[7] = { 0, 1, 2, 3, 4, 5, 6 };               // L O A D I N G

static int32 ps2LoadX, ps2LoadY, ps2LoadStep;

static inline uint16 ps2Dim(uint16 c) { return (uint16)((c >> 1) & 0x7BEF); }

static void ps2LoadPixel(int32 x, int32 y, uint16 c)
{
    if (x >= 0 && y >= 0 && x < screens[0].size.x && y < screens[0].size.y)
        screens[0].frameBuffer[y * screens[0].pitch + x] = c;
}

static void ps2LoadGlyph(int32 x, int32 y, int32 g, uint16 c)
{
    for (int32 row = 0; row < 7; ++row) {
        for (int32 col = 0; col < 5; ++col) {
            if (ps2LoadGlyphs[g][row] & (16 >> col))
                ps2LoadPixel(x + col, y + row, c);
        }
    }
}

// ---- Sonic's running animation, taken from the game's own data ----
// Read once, before the first scene load: Data/Sprites/Players/Sonic.bin lists the animations, Players/Sonic.gif is the sprite sheet. The frames of one
// animation are cut out of the sheet and kept (8-bit, with the sheet palette as RGB565) in one small block, because the stage pools are emptied by
// every scene load. Nothing of the game's art is stored anywhere but in memory while the game runs.
#define PS2_ANI_FRAMES 16
struct Ps2LoadAnimFrame {
    int16 w, h, pivX, pivY;
    uint32 off;
};
struct Ps2LoadAnim {
    int32 count;
    uint16 pal[256];
    Ps2LoadAnimFrame fr[PS2_ANI_FRAMES];
    uint8 pix[1];
};
static Ps2LoadAnim *ps2Sonic = NULL;
static bool32 ps2SonicTried   = false;
static bool32 ps2NameIs(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
    }
    return *a == *b;
}
void RSDK::PS2LoadingAnimInit()
{
    if (ps2SonicTried)
        return;
    ps2SonicTried = true;
    struct Raw {
        int16 sprX, sprY, w, h, pivX, pivY;
    } raw[PS2_ANI_FRAMES];
    int32 rawCount = 0;
    char sheetName[0x40] = "";
    FileInfo info;
    InitFileInfo(&info);
    if (!LoadFile(&info, "Data/Sprites/Players/Sonic.bin", FMODE_RB)) {
        ps2_log("[gfx] loading animation: Sonic.bin not found\n");
        return;
    }
    if (ReadInt32(&info, false) != RSDK_SIGNATURE_SPR) {
        CloseFile(&info);
        return;
    }
    ReadInt32(&info, false); // total frame count
    char tmp[0x40];
    const uint8 sheetCount = ReadInt8(&info);
    for (int32 s = 0; s < sheetCount; ++s) {
        ReadString(&info, tmp);
        if (s == 0)
            snprintf(sheetName, sizeof(sheetName), "%s", tmp);
    }
    const uint8 hitboxCount = ReadInt8(&info);
    for (int32 h = 0; h < hitboxCount; ++h) ReadString(&info, tmp);
    const int32 animCount = ReadInt16(&info);
    int32 picked          = -1;
    for (int32 a = 0; a < animCount && a < 64; ++a) {
        char name[0x40];
        ReadString(&info, name);
        const int32 frames = ReadInt16(&info);
        ReadInt16(&info); // speed
        ReadInt8(&info);  // loop index
        ReadInt8(&info);  // rotation style
        const bool32 take = picked < 0 && ps2NameIs(name, "Run");
        if (take)
            picked = a;
        ps2_log("[gfx] Sonic.bin animation %d '%s': %d frames%s\n", (int)a, name, (int)frames, take ? " <- used" : "");
        for (int32 f = 0; f < frames; ++f) {
            const int32 sheetID = ReadInt8(&info);
            ReadInt16(&info); // duration
            ReadInt16(&info); // unicode char
            Raw r;
            r.sprX = ReadInt16(&info);
            r.sprY = ReadInt16(&info);
            r.w    = ReadInt16(&info);
            r.h    = ReadInt16(&info);
            r.pivX = ReadInt16(&info);
            r.pivY = ReadInt16(&info);
            for (int32 h = 0; h < hitboxCount; ++h) {
                ReadInt16(&info);
                ReadInt16(&info);
                ReadInt16(&info);
                ReadInt16(&info);
            }
            if (take && sheetID == 0 && rawCount < PS2_ANI_FRAMES)
                raw[rawCount++] = r;
        }
    }
    CloseFile(&info);
    if (rawCount <= 0 || !sheetName[0]) {
        ps2_log("[gfx] loading animation: no running animation found in Sonic.bin\n");
        return;
    }
    uint32 total = 0;
    for (int32 i = 0; i < rawCount; ++i) total += (uint32)(raw[i].w * raw[i].h);
    Ps2LoadAnim *anim = (Ps2LoadAnim *)malloc(sizeof(Ps2LoadAnim) + total);
    if (!anim) {
        ps2_log("[gfx] loading animation: no memory for %u bytes\n", (unsigned)total);
        return;
    }
    char path[0x100];
    snprintf(path, sizeof(path), "Data/Sprites/%s", sheetName);
    ImageGIF image;
    bool32 ok = false;
    uint8 *sheet = NULL;
    if (image.Load(path, true) && image.width > 0 && image.height > 0) {
        AllocateStorage((void **)&sheet, image.width * image.height, DATASET_TMP, false);
        if (sheet) {
            image.pixels = sheet;
            ok           = image.Load(NULL, false) && image.palette;
        }
    }
    if (ok) {
        for (int32 c = 0; c < 256; ++c) {
            const uint32 rgb = image.palette[c];
            anim->pal[c]     = (uint16)((((rgb >> 16) & 0xFF) >> 3 << 11) | (((rgb >> 8) & 0xFF) >> 2 << 5) | ((rgb & 0xFF) >> 3));
        }
        uint32 off = 0;
        anim->count = rawCount;
        for (int32 i = 0; i < rawCount; ++i) {
            anim->fr[i].w    = raw[i].w;
            anim->fr[i].h    = raw[i].h;
            anim->fr[i].pivX = raw[i].pivX;
            anim->fr[i].pivY = raw[i].pivY;
            anim->fr[i].off  = off;
            for (int32 y = 0; y < raw[i].h; ++y) {
                for (int32 x = 0; x < raw[i].w; ++x) {
                    const int32 sx = raw[i].sprX + x, sy = raw[i].sprY + y;
                    anim->pix[off++] = (sx >= 0 && sy >= 0 && sx < image.width && sy < image.height) ? sheet[sy * image.width + sx] : 0;
                }
            }
        }
        ps2Sonic = anim;
        ps2_log("[gfx] loading animation: %d frames from %s, %u bytes kept\n", (int)rawCount, path, (unsigned)(sizeof(Ps2LoadAnim) + total));
    }
    else {
        free(anim);
        ps2_log("[gfx] loading animation: could not read %s\n", path);
    }
    image.Close();
    if (sheet)
        RemoveStorageEntry((void **)&sheet);
}
static int32 ps2LoadW = PS2_LOAD_W, ps2LoadH = PS2_LOAD_H; // size of the panel in use (the running Sonic needs a taller one than the ball)
static int32 ps2SonicLeft, ps2SonicTop, ps2SonicW, ps2SonicH; // bounding box of the frames relative to the animation origin
static void PS2LoadingHook(int phase)
{
    ScreenInfo *s = &screens[0];
    if (phase == 0) { // size the panel, then save the picture under it
        if (ps2Sonic && ps2Sonic->count > 0) {
            int32 l = 9999, t = 9999, r = -9999, b = -9999;
            for (int32 i = 0; i < ps2Sonic->count; ++i) {
                const Ps2LoadAnimFrame *fr = &ps2Sonic->fr[i];
                l = fr->pivX < l ? fr->pivX : l;
                t = fr->pivY < t ? fr->pivY : t;
                r = fr->pivX + fr->w > r ? fr->pivX + fr->w : r;
                b = fr->pivY + fr->h > b ? fr->pivY + fr->h : b;
            }
            ps2SonicLeft = l;
            ps2SonicTop  = t;
            ps2SonicW    = r - l;
            ps2SonicH    = b - t;
            ps2LoadW     = 6 + ps2SonicW + 6 + 7 * 6 + 3 * 3 + 6; // margin, Sonic, gap, "LOADING", three dots, margin
            ps2LoadH     = ps2SonicH + 8 > 22 ? ps2SonicH + 8 : 22;
        }
        else {
            ps2LoadW = PS2_LOAD_W;
            ps2LoadH = PS2_LOAD_H;
        }
        // inside the title-safe area: a TV (and the picture the GS shows) hides the outer 8% or so
        ps2LoadX    = s->size.x - ps2LoadW - s->size.x / 12 - 4;
        ps2LoadY    = s->size.y - ps2LoadH - s->size.y / 12 - 2;
        ps2LoadStep = 0;
        ps2_log("[gfx] loading panel: screen %dx%d, panel %dx%d at %d,%d; Sonic box %dx%d from %d,%d\n", (int)s->size.x, (int)s->size.y, (int)ps2LoadW, (int)ps2LoadH, (int)ps2LoadX, (int)ps2LoadY, (int)ps2SonicW, (int)ps2SonicH, (int)ps2SonicLeft, (int)ps2SonicTop);
        for (int32 y = 0; y < ps2LoadH; ++y)
            for (int32 x = 0; x < ps2LoadW; ++x) screens[SCREEN_COUNT - 1].frameBuffer[y * ps2LoadW + x] = s->frameBuffer[(ps2LoadY + y) * s->pitch + ps2LoadX + x];
        return;
    }
    // put the saved picture back (phase 1 draws a new frame on it, phase 2 leaves it clean)
    for (int32 y = 0; y < ps2LoadH; ++y)
        for (int32 x = 0; x < ps2LoadW; ++x) s->frameBuffer[(ps2LoadY + y) * s->pitch + ps2LoadX + x] = screens[SCREEN_COUNT - 1].frameBuffer[y * ps2LoadW + x];
    if (phase == 2) {
        ps2_gfx_present(s->frameBuffer, s->pitch, s->size.x, s->size.y);
        return;
    }
    // dark translucent panel
    for (int32 y = 0; y < ps2LoadH; ++y)
        for (int32 x = 0; x < ps2LoadW; ++x) {
            uint16 *px = &s->frameBuffer[(ps2LoadY + y) * s->pitch + ps2LoadX + x];
            *px        = ps2Dim(ps2Dim(*px));
        }
    int32 tx;
    if (ps2Sonic && ps2Sonic->count > 0) {
        // Sonic running in place, one animation frame per call (about every 90 ms)
        const Ps2LoadAnimFrame *fr = &ps2Sonic->fr[ps2LoadStep % ps2Sonic->count];
        const int32 ox             = ps2LoadX + 6 - ps2SonicLeft + fr->pivX; // frame top-left on screen
        const int32 oy             = ps2LoadY + (ps2LoadH - ps2SonicH) / 2 - ps2SonicTop + fr->pivY;
        const uint8 *src           = ps2Sonic->pix + fr->off;
        for (int32 y = 0; y < fr->h; ++y) {
            for (int32 x = 0; x < fr->w; ++x, ++src) {
                if (*src)
                    ps2LoadPixel(ox + x, oy + y, ps2Sonic->pal[*src]);
            }
        }
        tx = ps2LoadX + 6 + ps2SonicW + 6;
    }
    else {
        // no Sonic frames (the data could not be read): the old rolling ball, radius 8 at the left of the panel
        const int32 cx = ps2LoadX + 12, cy = ps2LoadY + ps2LoadH / 2;
        const float ang = ps2LoadStep * 0.6f;
        const float ca = cosf(ang), sa = sinf(ang);
        for (int32 y = -8; y <= 8; ++y) {
            for (int32 x = -8; x <= 8; ++x) {
                const float r2 = (float)(x * x + y * y);
                if (r2 > 64.0f)
                    continue;
                const float u = x * ca + y * sa; // coordinate across the bands
                uint16 c      = (((int32)floorf(u * 0.5f + 16.0f)) & 1) ? 0x1B7F : 0x0A5C; // light / dark blue (RGB565)
                if (r2 > 49.0f)
                    c = 0x0210; // darker rim
                if (x >= 2 && x <= 5 && y >= -3 && y <= -1)
                    c = 0xFFFF; // glint
                ps2LoadPixel(cx + x, cy + y, c);
            }
        }
        tx = ps2LoadX + 26;
    }
    // LOADING and 0-3 dots
    const int32 ty = ps2LoadY + (ps2LoadH - 7) / 2;
    for (int32 i = 0; i < 7; ++i, tx += 6) ps2LoadGlyph(tx, ty, ps2LoadWord[i], 0xFFFF);
    for (int32 i = 0; i < (ps2LoadStep & 3); ++i, tx += 3) ps2LoadGlyph(tx, ty, 7, 0xFFFF);
    ++ps2LoadStep;
    ps2_gfx_present(s->frameBuffer, s->pitch, s->size.x, s->size.y);
}bool RenderDevice::Init()
{
    ps2_stage("render device init");
    PrintLog(PRINT_NORMAL, "PS2 render device init");

    // Video options (Options > Video) are stored in the PC fields of videoSettings and read every frame in FlipScreen:
    //   windowed == false -> 480 lines, true -> 240p      vsync -> progressive scan      tripleBuffered -> 16:9      bordered -> letterbox
    //   shaderID != 0 -> filtered (bilinear), 0 -> clean (nearest)
    videoSettings.windowWidth  = 640;
    videoSettings.windowHeight = 448;

    // The render width follows the Aspect ratio setting: 424x240 for 16:9 (the stock widescreen size), 320x240 for 4:3 (square pixels on a 4:3
    // TV; the game's UI lays out from ScreenInfo->size.x). It is re-applied whenever the setting changes (see FlipScreen).
    ps2_gfx_configure(!videoSettings.windowed, videoSettings.vsync, videoSettings.tripleBuffered, videoSettings.bordered, videoSettings.shaderID != 0);
    videoSettings.pixWidth = ps2_gfx_wide() ? PS2_WIDE_PIXWIDTH : PS2_NARROW_PIXWIDTH;

    ps2_set_loading_hook(PS2LoadingHook);
    ps2_stage("render device: frame buffers, then audio start");
    if (!SetupRendering() || !AudioDevice::Init())
        return false;

    InitInputDevices();
    ps2_log("[mem] render init done: heap in use %u KB\n", (unsigned)(ps2_heap_used() / 1024));
    return true;
}

void RenderDevice::CopyFrameBuffer()
{
    // Nothing to do: FlipScreen reads the engine's frame buffer directly.
}

// Special stages (UFO*) and Blue Spheres are the heaviest scenes (software 3D / Mode-7 floors fill every pixel several times). In 16:9 they are
// rendered 320 pixels wide instead of 424 and stretched across the 16:9 picture by the GS, a quarter fewer pixels to draw. The width is chosen
// when the scene loads (before its objects run StageLoad), so the HUD and cameras lay out for the width they are drawn at.
static bool32 ps2SceneLowRes = false;

static int32 ps2WantedRenderWidth() { return (ps2_gfx_wide() && !ps2SceneLowRes) ? PS2_WIDE_PIXWIDTH : PS2_NARROW_PIXWIDTH; }

void RSDK::PS2ApplySceneRenderWidth()
{
    ps2SceneLowRes = strncmp(currentSceneFolder, "UFO", 3) == 0 || strcmp(currentSceneFolder, "SpecialBS") == 0;
    const int32 wantW = ps2WantedRenderWidth();
    if (screens[0].size.x != wantW) {
        videoSettings.pixWidth = wantW;
        for (int32 s = 0; s < 4 && s < SCREEN_COUNT; ++s) SetScreenSize(s, wantW, screens[s].size.y);
        RenderDevice::pixelSize.x = screens[0].size.x;
        RenderDevice::pixelSize.y = screens[0].size.y;
        ps2_log("[gfx] render size now %dx%d for scene %s\n", (int)screens[0].size.x, (int)screens[0].size.y, currentSceneFolder);
    }
}
void RenderDevice::FlipScreen()
{
    if (windowRefreshDelay > 0) {
        windowRefreshDelay--;
        if (!windowRefreshDelay)
            UpdateGameWindow();
        return;
    }

    ps2_gfx_configure(!videoSettings.windowed, videoSettings.vsync, videoSettings.tripleBuffered, videoSettings.bordered, videoSettings.shaderID != 0);

    // Aspect ratio changed (or a developer override pinned a different one): resize the render screens. This only updates geometry
    // (size, pitch, centre, clip bounds); the picture is redrawn from scratch every frame.
    const int32 wantW = ps2WantedRenderWidth();
    if (screens[0].size.x != wantW) {
        videoSettings.pixWidth = wantW;
        for (int32 s = 0; s < 4 && s < SCREEN_COUNT; ++s) SetScreenSize(s, wantW, screens[s].size.y);
        pixelSize.x = screens[0].size.x;
        pixelSize.y = screens[0].size.y;
        ps2_log("[gfx] render size now %dx%d\n", (int)screens[0].size.x, (int)screens[0].size.y);
    }

    // Adaptive 3D detail. Work time = the time from the end of the last present to here (the present itself waits for vblank, so frame times
    // are quantised to 16.7 ms steps and cannot show headroom). Too slow -> drop faces smaller than 1, 2 or 3 pixels in the 3D scenes (special
    // stages); plenty of headroom -> bring them back. Developer flag boot.txt lod=0..3 pins a level, lod=a (the default) adapts.
    static uint64 tReady     = 0;
    static uint32 workSum    = 0, workFrames = 0;
    const uint64 tNow        = ps2_time_us();
    if (tReady) {
        workSum += (uint32)(tNow - tReady);
        if (++workFrames >= 20) {
            const uint32 avg = workSum / workFrames; // microseconds of work per frame, not counting the present
            if (ps2_lod_pinned() >= 0)
                PS2SetS3DLevel(ps2_lod_pinned());
            // The audio mixer runs after this point in the same frame (about 3-5 ms with music playing, see docs section 12), so the engine's own
            // share of the 16.7 ms is about 12 ms: step the detail down above that, back up only with real headroom.
            else if (avg > (uint32)ps2_tune(4, 12000) && ps2S3dLevel < 6)
                PS2SetS3DLevel(ps2S3dLevel + 1);
            else if (avg < (uint32)ps2_tune(5, 9500) && ps2S3dLevel > 0)
                PS2SetS3DLevel(ps2S3dLevel - 1);
            workSum = workFrames = 0;
        }
    }
    ps2_gfx_present(screens[0].frameBuffer, screens[0].pitch, screens[0].size.x, screens[0].size.y);
    tReady = ps2_time_us();

    // memory diagnostics for the 32MB budget: first frame, then every ~5 seconds
    static uint32 frameCounter = 0;
    if (frameCounter == 0 || frameCounter % 300 == 0)
    {
        ps2_log("[mem] frame %u: heap in use %u KB of %u KB total EE RAM\n", (unsigned)frameCounter, (unsigned)(ps2_heap_used() / 1024),
                (unsigned)(ps2_ram_total() / 1024));
        PS2StorageReport("periodic");
    }
    if (ps2_strstress() && frameCounter == 200)
        PS2StringStress();
    PS2VerifyStorage("end of frame", frameCounter);

    if (ps2_achtest() && frameCounter >= 700 && (frameCounter % 600) == 100 && !achievementList.empty()) {
        // developer test: force a banner for the next achievement in the list (as if just earned)
        static uint32 nextAch = 0;
        const uint32 idx = nextAch++ % achievementList.size();
        achievementList[idx].achieved = true;
        achievementStack.push_back((int32)idx);
        ps2_log("[achtest] banner for achievement %u at frame %u\n", (unsigned)idx, (unsigned)frameCounter);
    }
    frameCounter++;
}

void RenderDevice::Release(bool32 isRefresh)
{
    if (!isRefresh) {
        if (displayInfo.displays)
            free(displayInfo.displays);
        displayInfo.displays = NULL;

        if (scanlines)
            free(scanlines);
        scanlines = NULL;

        ps2_gfx_shutdown();
    }
}

void RenderDevice::RefreshWindow()
{
    // Fixed output mode: nothing to refresh.
}

void RenderDevice::GetWindowSize(int32 *width, int32 *height)
{
    if (width)
        *width = 640;
    if (height)
        *height = 448;
}

void RenderDevice::SetupImageTexture(int32 width, int32 height, uint8 *imagePixels) {}
void RenderDevice::SetupVideoTexture_YUV420(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                            int32 strideV)
{
}
void RenderDevice::SetupVideoTexture_YUV422(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                            int32 strideV)
{
}
void RenderDevice::SetupVideoTexture_YUV444(int32 width, int32 height, uint8 *yPlane, uint8 *uPlane, uint8 *vPlane, int32 strideY, int32 strideU,
                                            int32 strideV)
{
}

bool RenderDevice::ProcessEvents() { return true; }

// FlipScreen blocks on vertical sync, so the engine loop is already paced to the display (60 Hz NTSC).
void RenderDevice::InitFPSCap() {}
bool RenderDevice::CheckFPSCap() { return true; }
void RenderDevice::UpdateFPSCap() {}

void RenderDevice::LoadShader(const char *fileName, bool32 linear) { PrintLog(PRINT_NORMAL, "This render device does not support shaders!"); }

bool RenderDevice::InitShaders()
{
    shaderCount = 1;
    for (int32 s = 0; s < SHADER_COUNT; ++s) shaderList[s].linear = true;
    videoSettings.shaderID = 0;
    return true;
}

void RenderDevice::GetDisplays()
{
    displayCount = 1;

    if (displayInfo.displays)
        free(displayInfo.displays);

    displayInfo.displays               = (decltype(displayInfo.displays))malloc(sizeof(displayInfo.displays[0]));
    displayInfo.displays[0].width        = 640;
    displayInfo.displays[0].height       = 448;
    displayInfo.displays[0].refresh_rate = 60;

    displayWidth[0]  = 640;
    displayHeight[0] = 448;

    videoSettings.fsWidth     = 0;
    videoSettings.fsHeight    = 0;
    videoSettings.refreshRate = 60;
}

bool RenderDevice::InitGraphicsAPI()
{
    videoSettings.shaderSupport = false;

    viewSize.x = 640;
    viewSize.y = 448;

    int32 maxPixHeight = 0;
    int32 screenWidth  = videoSettings.pixWidth;

    for (int32 s = 0; s < 4 && s < SCREEN_COUNT; ++s) {
        if (videoSettings.pixHeight > maxPixHeight)
            maxPixHeight = videoSettings.pixHeight;

        screens[s].size.y = videoSettings.pixHeight;

        if (screenWidth > SCREEN_XMAX)
            screenWidth = SCREEN_XMAX;

        memset(&screens[s].frameBuffer, 0, sizeof(screens[s].frameBuffer));
        SetScreenSize(s, screenWidth, screens[s].size.y);
    }

    pixelSize.x = screens[0].size.x;
    pixelSize.y = screens[0].size.y;

    textureSize.x = 512.0;
    textureSize.y = 256.0;

    lastShaderID = -1;
    engine.inFocus          = 1;
    videoSettings.viewportX = 0;
    videoSettings.viewportY = 0;
    videoSettings.viewportW = 1.0 / viewSize.x;
    videoSettings.viewportH = 1.0 / viewSize.y;

    return ps2_gfx_init((int)textureSize.x, (int)textureSize.y) != 0;
}

bool RenderDevice::SetupRendering()
{
    GetDisplays();

    if (!InitGraphicsAPI() || !InitShaders())
        return false;

    // Sized for the widest render width: the width can change at runtime (Aspect ratio setting), so it must not depend on the current one.
    int32 size = SCREEN_XMAX >= SCREEN_YSIZE ? SCREEN_XMAX : SCREEN_YSIZE;
    scanlines  = (ScanlineInfo *)malloc(size * sizeof(ScanlineInfo));
    memset(scanlines, 0, size * sizeof(ScanlineInfo));

    videoSettings.windowState = WINDOWSTATE_ACTIVE;
    videoSettings.dimMax      = 1.0;
    videoSettings.dimPercent  = 1.0;

    return true;
}
