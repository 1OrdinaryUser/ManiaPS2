// PS2 audio device: the engine's float mixer runs on the EE once per frame and the result is queued to audsrv (IOP/SPU2) as S16 stereo.
extern "C" void ps2_audio_lock(void);
extern "C" void ps2_audio_unlock(void);
#define LockAudioDevice()   ps2_audio_lock()
#define UnlockAudioDevice() ps2_audio_unlock()

namespace RSDK
{
class AudioDevice : public AudioDeviceBase
{
public:
    static bool32 Init();
    static void Release();

    // Called once per engine frame: tops up the audsrv ring buffer with freshly mixed audio.
    static void FrameInit();
    static void Pump(); // tops up the audsrv ring buffer (called by the HAL while waiting for vsync)
    static void PumpIfLate();
    static void MixerStep(); // mixer thread: tops up the ring
    static void MainTick();  // game thread, once per frame: reports on the mixer thread // same, from the middle of a frame, but only when the last mix is overdue (a long frame)

    inline static void HandleStreamLoad(ChannelInfo *channel, bool32 async)
    {
        // No threads on this backend: always load synchronously.
        LoadStream(channel);
    }

private:
    static uint8 contextInitialized;
    static bool32 outputReady;

    static void InitAudioChannels();
};
} // namespace RSDK