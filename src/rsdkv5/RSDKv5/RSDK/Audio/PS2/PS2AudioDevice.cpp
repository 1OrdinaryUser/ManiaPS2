uint8 AudioDevice::contextInitialized;
bool32 AudioDevice::outputReady;
static bool32 threaded = false; // the mixer thread (ps2_audio_thread_start) does the mixing

bool32 AudioDevice::Init()
{
    if (!contextInitialized) {
        contextInitialized = true;
        InitAudioChannels();
    }

    outputReady = ps2_audio_init(AUDIO_FREQUENCY) != 0;
    audioState  = outputReady;
    if (outputReady) {
        if (ps2_audio_thread_start(AudioDevice::MixerStep)) {
            threaded = true; // mixing happens in the mixer thread; the frame loop only reports on it (once per frame, while waiting for vsync)
            ps2_gfx_set_idle_hook(AudioDevice::MainTick);
        }
        else {
            ps2_gfx_set_idle_hook(AudioDevice::Pump);
            ps2_gfx_set_mid_hook(AudioDevice::PumpIfLate);
        }
    }
    return true;
}

void AudioDevice::Release() {}

void AudioDevice::InitAudioChannels() { AudioDeviceBase::InitAudioChannels(); }

// Runs from the HAL while the EE would be waiting for vertical sync, so the mixing costs (almost) no frame time. FrameInit() is left empty.
void AudioDevice::FrameInit() {}

// Audio is mixed in small slices so that no single piece of work is bigger than the idle time left in a frame.
// A slice is PS2_MIX_SLICE floats = PS2_MIX_SLICE / 2 stereo frames (256 frames = ~5.8 ms of audio, 1KB of PCM) and costs about 1 ms
// (the Vorbis decode is done in matching 512-float steps, see PS2DecodeStreamTo in Audio.cpp). Before this, audio was mixed in 2048-float
// chunks costing 4-8.6 ms each, which overran the vblank whenever the engine left less idle time than that (see docs/STATUS_AND_PLAN.md).
#define PS2_MIX_SLICE 512
#define PS2_MAX_SLICES_PER_FRAME 8
// (Capping voluntary slices at 3 per frame was tried 2026-10-17 and made things worse: the ring starved, urgent slices rose from ~0-20 to
// ~1200 per 600 frames and on-time menu frames fell to 263-288 of 300. So voluntary mixing is limited only by the time left in the frame.)
#define PS2_MAX_VOLUNTARY_SLICES PS2_MAX_SLICES_PER_FRAME

// The ring holds about 106 ms of audio. Mixing used to happen once per frame, in the time left before vsync, and at most 4 forced slices (23 ms of
// audio) per frame: fine at 60 fps, but a frame longer than about 45 ms (a console slower than the emulator, a heavy scene) drained the ring and the sound
// fell apart. Now a mix is also done from the middle of a long frame (PumpIfLate, called by the engine between draw groups, layers and logic steps)
// once the last one is more than PS2_LATE_US old, and a late Pump refills the ring further.
#define PS2_LATE_US 24000u
#define PS2_LATE_MAX_SLICES 8
static uint32 ps2LastPumpCycles = 0;

void AudioDevice::PumpIfLate()
{
    if (!outputReady || ps2_noaudio() || threaded)
        return;
    if ((ps2_cycles() - ps2LastPumpCycles) / 295u >= PS2_LATE_US)
        Pump();
}

// ---- mixer thread ----
// MixerStep runs in the mixer thread every 5 ms: it fills whatever space the audsrv ring has with freshly mixed slices, holding the audio lock (so the
// game thread can not change channels or free sounds in the middle of a slice). It must not log (see ps2_log) or touch the screen.
static volatile uint32 ps2MixWakes = 0, ps2MixSlices = 0, ps2MixUs = 0, ps2MixEmpty = 0;
static volatile int32 ps2MixMaxFree = 0;
void AudioDevice::MixerStep()
{
    if (!outputReady || ps2_noaudio())
        return;
    static float mix[PS2_MIX_SLICE];
    static int16 pcm[PS2_MIX_SLICE];
    ps2_audio_lock();
    if (!ps2_audio_is_paused()) {
        int freeBytes = ps2_audio_free_bytes();
        ++ps2MixWakes;
        if (freeBytes > ps2MixMaxFree)
            ps2MixMaxFree = freeBytes;
        if (freeBytes >= 16 * 1024) // the ring holds about 18.8 KB: this much free means under 16 ms of sound is left
            ++ps2MixEmpty;
        for (int32 n = 0; n < 24 && freeBytes >= (int)sizeof(pcm); ++n) {
            const uint32 t0 = ps2_cycles();
            ProcessAudioMixing(mix, PS2_MIX_SLICE);
            for (int32 i = 0; i < PS2_MIX_SLICE; ++i) {
                float v = mix[i] * 32767.0f;
                if (v > 32767.0f)
                    v = 32767.0f;
                else if (v < -32768.0f)
                    v = -32768.0f;
                pcm[i] = (int16)v;
            }
            ps2_audio_write(pcm, sizeof(pcm));
            freeBytes -= (int)sizeof(pcm);
            ++ps2MixSlices;
            ps2MixUs += (ps2_cycles() - t0) / 295u;
        }
    }
    ps2_audio_unlock();
}
extern "C" void ps2_audio_note_mix(unsigned wakes, unsigned slices, unsigned avgUs, int maxFree, unsigned empty);
// Called once per frame from the HAL (game thread): every 300 frames, logs what the mixer thread did.
void AudioDevice::MainTick()
{
    static uint32 frames = 0;
    if ((++frames % 300) != 0)
        return;
    const uint32 wakes = ps2MixWakes, slices = ps2MixSlices, us = ps2MixUs, empty = ps2MixEmpty;
    const int32 maxFree = ps2MixMaxFree;
    ps2MixWakes = ps2MixSlices = ps2MixUs = ps2MixEmpty = 0;
    ps2MixMaxFree = 0;
    const uint32 avg = slices ? us / slices : 0;
    ps2_audio_note_mix(wakes, slices, avg, maxFree, empty);
    ps2_log("[audio] mixer thread over 300 frames: %u wakes, %u slices (%u us each), ring free at most %d bytes, %u wakes with the ring nearly empty\n",
            (unsigned)wakes, (unsigned)slices, (unsigned)avg, (int)maxFree, (unsigned)empty);
}
void AudioDevice::Pump()
{
    if (!outputReady || ps2_noaudio() || threaded)
        return;

    const uint32 pumpStart = ps2_cycles();
    const bool32 late      = ps2LastPumpCycles && (pumpStart - ps2LastPumpCycles) / 295u >= PS2_LATE_US;
    ps2LastPumpCycles      = pumpStart;

    static float mix[PS2_MIX_SLICE];
    static int16 pcm[PS2_MIX_SLICE];

    static uint32 slicesQueued = 0, frames = 0;
    static float peak = 0.0f;
    static uint32 mixUs = 0, convUs = 0, writeUs = 0, rpcUs = 0, timed = 0;

    // Slice cost is bimodal: stb_vorbis decodes whole packets (~1024 frames, ~3.6 ms) and hands out the buffered samples slice by slice, so a
    // slice is cheap (~0.3 ms) while samples are buffered and expensive when it triggers the next packet decode. The decoder's own counters say
    // which kind comes next, so each kind gets its own moving average (single outliers clamped) and its own gate.
    // A decaying *maximum* was tried first: one 97 ms outlier (a host-file stall while streaming music) kept the estimate inflated for hundreds of
    // slices, the gate then refused to mix, the ring drained and the urgent path mixed in bursts, which made more frames late.
    static uint32 cheapCostUs = 500, decodeCostUs = 3800;
    static uint32 sliceCostUs = 1500; // overall average, for the log only

    // Health counters, reported with the periodic [audio] line: how empty the ring got, and how often the urgent/deferred paths ran.
    static int32 maxFreeSeen = 0;
    static uint32 urgentCount = 0, deferredCount = 0, emptyRingFrames = 0;

    // One query of the ring per frame (it is an IOP RPC); afterwards the free space is tracked locally. It can only have grown meanwhile.
    const uint32 tA = ps2_cycles();
    int freeBytes   = ps2_audio_free_bytes();
    rpcUs += (ps2_cycles() - tA) / 295u;

    if (freeBytes > maxFreeSeen)
        maxFreeSeen = freeBytes;
    // The audsrv ring holds about 18.8KB (~106 ms); free space above ~16KB means under 2.8KB (16 ms) is left to play: an underrun is close.
    if (freeBytes >= 16 * 1024)
        ++emptyRingFrames;

    // Below this much free space the ring holds more than ~60 ms of audio. Above it, mix regardless of the time left (a crackle is worse than
    // a late frame). The slices are small, so even the forced ones are short.
    // A late pump is behind: refill to a fuller ring, and more slices at once.
    const int urgentFree = late ? 4096 : ps2_tune(0, 8192); // tune0 (developer A/B)
    const int32 urgentCap = late ? PS2_LATE_MAX_SLICES : ps2_tune(3, 4);

    int32 urgentThisFrame = 0;
    for (int32 slice = 0; slice < (late ? PS2_LATE_MAX_SLICES : PS2_MAX_SLICES_PER_FRAME); ++slice) {
        if (freeBytes < (int)sizeof(pcm))
            break;

        const bool32 urgent = freeBytes >= urgentFree;

        // Will this slice have to decode a new Vorbis packet? (A slice consumes PS2_MIX_SLICE / 2 stereo frames from the decoder.)
        bool32 musicPlaying = false;
        for (int32 c = 0; c < CHANNEL_COUNT; ++c) {
            if (channels[c].state == CHANNEL_STREAM) {
                musicPlaying = true;
                break;
            }
        }
        const bool32 willDecode = musicPlaying && vorbisInfo && (vorbisInfo->channel_buffer_end - vorbisInfo->channel_buffer_start) < (PS2_MIX_SLICE / 2);
        const uint32 expectedUs = willDecode ? decodeCostUs : cheapCostUs;

        if (urgent) {
            // Forced slices ignore the time budget, so cap them per frame: a deep hole in the ring is refilled over two or three frames
            // instead of in one long burst that would itself make the frame late.
            if (urgentThisFrame >= urgentCap) // tune3
                break;
            ++urgentThisFrame;
            ++urgentCount;
        }
        else if (slice >= PS2_MAX_VOLUNTARY_SLICES || ps2_vblank_remaining_us() < expectedUs * (uint32)ps2_tune(1, 150) / 100u + (uint32)ps2_tune(2, 300)) { // tune1 = percent of the expected cost, tune2 = extra us
            if (slice == 0)
                ++deferredCount;
            break;
        }

        const uint32 t0 = ps2_cycles();

        ps2_stage("audio: mixing (music decode, effects)");
        ProcessAudioMixing(mix, PS2_MIX_SLICE);
        const uint32 tMix = ps2_cycles();
        mixUs += (tMix - t0) / 295u;

        for (int32 i = 0; i < PS2_MIX_SLICE; ++i) {
            float v = mix[i] * 32767.0f;
            if (mix[i] > peak)
                peak = mix[i];
            if (v > 32767.0f)
                v = 32767.0f;
            else if (v < -32768.0f)
                v = -32768.0f;
            pcm[i] = (int16)v;
        }

        const uint32 tConv = ps2_cycles();
        convUs += (tConv - tMix) / 295u;
        ps2_stage("audio: writing to the sound ring (audsrv)");
        ps2_audio_write(pcm, sizeof(pcm));
        writeUs += (ps2_cycles() - tConv) / 295u;
        freeBytes -= (int)sizeof(pcm);
        ++timed;
        ++slicesQueued;
        const uint32 cost = (ps2_cycles() - t0) / 295u;
        const uint32 clamped = cost > 6000u ? 6000u : cost;
        sliceCostUs          = (sliceCostUs * 7 + clamped) / 8;
        if (willDecode)
            decodeCostUs = (decodeCostUs * 7 + clamped) / 8;
        else
            cheapCostUs = (cheapCostUs * 7 + clamped) / 8;
    }

    if ((++frames % 600) == 0) {
        int32 active = 0;
        for (int32 c = 0; c < CHANNEL_COUNT; ++c) active += channels[c].state != CHANNEL_IDLE;
        ps2_log("[audio] %u slices queued in %u frames, peak %.3f, %d channels active, ring free %d bytes | per slice: mix %u us, convert %u us, write %u us, est typical %u us; free-query %u us/call\n",
                (unsigned)slicesQueued, (unsigned)frames, peak, (int)active, ps2_audio_free_bytes(), (unsigned)(mixUs / (timed ? timed : 1)),
                (unsigned)(convUs / (timed ? timed : 1)), (unsigned)(writeUs / (timed ? timed : 1)), (unsigned)sliceCostUs, (unsigned)(rpcUs / (frames ? frames : 1)));
        ps2_log("[audio]   ring health over %u frames: max free %d bytes, frames with ring nearly empty %u, urgent slices %u, deferred (no time) %u\n",
                (unsigned)600, (int)maxFreeSeen, (unsigned)emptyRingFrames, (unsigned)urgentCount, (unsigned)deferredCount);
        ps2_log("[audio]   music file reads: %u cache refills, total %u us, worst %u us\n", (unsigned)ps2RefillCount, (unsigned)ps2RefillTotalUs,
                (unsigned)ps2RefillMaxUs);
        ps2RefillCount = ps2RefillTotalUs = ps2RefillMaxUs = 0;
        maxFreeSeen = 0;
        urgentCount = deferredCount = emptyRingFrames = 0;
        mixUs = convUs = writeUs = rpcUs = timed = 0;
        peak = 0.0f;
    }
}
