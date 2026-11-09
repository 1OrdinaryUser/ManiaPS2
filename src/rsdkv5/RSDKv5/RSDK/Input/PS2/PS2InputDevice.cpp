#include "../../PS2/ps2_hal.h"

using namespace RSDK;

static float PS2NormaliseAxis(uint8 raw)
{
    // 0..255, centre ~128 -> -1..1 with a small hardware deadzone
    float v = ((float)raw - 128.0f) / 128.0f;
    if (v > 1.0f)
        v = 1.0f;
    if (v > -0.15f && v < 0.15f)
        return 0.0f;
    return v;
}

void RSDK::SKU::InputDevicePS2::UpdateInput()
{
    ps2_pad_state pad;
    ps2_pad_read(this->port, &pad);

    if (this->port != 0)
        this->active = pad.connected != 0; // port 2 counts as a device only while a pad is plugged in

    this->prevButtonMasks = this->buttonMasks;

    float prevHDeltaL = hDelta_L, prevVDeltaL = vDelta_L, prevHDeltaR = hDelta_R, prevVDeltaR = vDelta_R;
    float prevTriggerDeltaL = triggerDeltaL, prevTriggerDeltaR = triggerDeltaR;

    struct {
        uint16 ps2;
        int32 mask;
    } static const map[] = {
        { PS2_PAD_UP, KEYMASK_UP },        { PS2_PAD_DOWN, KEYMASK_DOWN },     { PS2_PAD_LEFT, KEYMASK_LEFT },
        { PS2_PAD_RIGHT, KEYMASK_RIGHT },  { PS2_PAD_CROSS, KEYMASK_A },       { PS2_PAD_CIRCLE, KEYMASK_B },
        { PS2_PAD_SQUARE, KEYMASK_X },     { PS2_PAD_TRIANGLE, KEYMASK_Y },    { PS2_PAD_START, KEYMASK_START },
        { PS2_PAD_SELECT, KEYMASK_SELECT }, { PS2_PAD_L3, KEYMASK_STICKL },    { PS2_PAD_R3, KEYMASK_STICKR },
        { PS2_PAD_L1, KEYMASK_BUMPERL },   { PS2_PAD_R1, KEYMASK_BUMPERR },
    };

    this->buttonMasks = 0;
    if (pad.connected) {
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
            if (pad.buttons & map[i].ps2)
                this->buttonMasks |= map[i].mask;
        }
        hDelta_L = PS2NormaliseAxis(pad.lx);
        vDelta_L = -PS2NormaliseAxis(pad.ly);
        hDelta_R = PS2NormaliseAxis(pad.rx);
        vDelta_R = -PS2NormaliseAxis(pad.ry);
    }
    else {
        hDelta_L = vDelta_L = hDelta_R = vDelta_R = 0.0f;
    }

    triggerDeltaL = (pad.connected && (pad.buttons & PS2_PAD_L2)) ? 1.0f : 0.0f;
    triggerDeltaR = (pad.connected && (pad.buttons & PS2_PAD_R2)) ? 1.0f : 0.0f;
    bumperDeltaL  = (this->buttonMasks & KEYMASK_BUMPERL) != 0 ? 1.0f : 0.0f;
    bumperDeltaR  = (this->buttonMasks & KEYMASK_BUMPERR) != 0 ? 1.0f : 0.0f;

    int32 changedButtons = ~this->prevButtonMasks & (this->prevButtonMasks ^ this->buttonMasks);

    if (changedButtons)
        ps2_log("[pad] pressed mask 0x%x (all 0x%x) scene=%s state=%d\n", (unsigned)changedButtons, (unsigned)this->buttonMasks, currentSceneFolder, (int)sceneInfo.state);

    if (changedButtons || hDelta_L != prevHDeltaL || vDelta_L != prevVDeltaL || hDelta_R != prevHDeltaR || vDelta_R != prevVDeltaR
        || triggerDeltaL != prevTriggerDeltaL || triggerDeltaR != prevTriggerDeltaR) {
        this->inactiveTimer[0] = 0;
        this->anyPress         = true;
    }
    else {
        ++this->inactiveTimer[0];
        this->anyPress = false;
    }

    if ((changedButtons & KEYMASK_A) || (changedButtons & KEYMASK_START))
        this->inactiveTimer[1] = 0;
    else
        ++this->inactiveTimer[1];

    this->stateUp       = (this->buttonMasks & KEYMASK_UP) != 0;
    this->stateDown     = (this->buttonMasks & KEYMASK_DOWN) != 0;
    this->stateLeft     = (this->buttonMasks & KEYMASK_LEFT) != 0;
    this->stateRight    = (this->buttonMasks & KEYMASK_RIGHT) != 0;
    this->stateA        = (this->buttonMasks & KEYMASK_A) != 0;
    this->stateB        = (this->buttonMasks & KEYMASK_B) != 0;
    this->stateC        = 0;
    this->stateX        = (this->buttonMasks & KEYMASK_X) != 0;
    this->stateY        = (this->buttonMasks & KEYMASK_Y) != 0;
    this->stateZ        = 0;
    this->stateStart    = (this->buttonMasks & KEYMASK_START) != 0;
    this->stateSelect   = (this->buttonMasks & KEYMASK_SELECT) != 0;
    this->stateBumper_L = (this->buttonMasks & KEYMASK_BUMPERL) != 0;
    this->stateBumper_R = (this->buttonMasks & KEYMASK_BUMPERR) != 0;
    this->stateStick_L  = (this->buttonMasks & KEYMASK_STICKL) != 0;
    this->stateStick_R  = (this->buttonMasks & KEYMASK_STICKR) != 0;

    ProcessInput(CONT_ANY);
}

void RSDK::SKU::InputDevicePS2::ProcessInput(int32 controllerID)
{
    controller[controllerID].keyUp.press |= this->stateUp;
    controller[controllerID].keyDown.press |= this->stateDown;
    controller[controllerID].keyLeft.press |= this->stateLeft;
    controller[controllerID].keyRight.press |= this->stateRight;
    controller[controllerID].keyA.press |= this->stateA;
    controller[controllerID].keyB.press |= this->stateB;
    controller[controllerID].keyC.press |= this->stateC;
    controller[controllerID].keyX.press |= this->stateX;
    controller[controllerID].keyY.press |= this->stateY;
    controller[controllerID].keyZ.press |= this->stateZ;
    controller[controllerID].keyStart.press |= this->stateStart;
    controller[controllerID].keySelect.press |= this->stateSelect;

#if RETRO_REV02
    stickL[controllerID].keyStick.press |= this->stateStick_L;
    stickL[controllerID].hDelta = this->hDelta_L;
    stickL[controllerID].vDelta = this->vDelta_L;
    stickL[controllerID].keyUp.press |= this->vDelta_L > INPUT_DEADZONE;
    stickL[controllerID].keyDown.press |= this->vDelta_L < -INPUT_DEADZONE;
    stickL[controllerID].keyLeft.press |= this->hDelta_L < -INPUT_DEADZONE;
    stickL[controllerID].keyRight.press |= this->hDelta_L > INPUT_DEADZONE;

    stickR[controllerID].keyStick.press |= this->stateStick_R;
    stickR[controllerID].hDelta = this->hDelta_R;
    stickR[controllerID].vDelta = this->vDelta_R;
    stickR[controllerID].keyUp.press |= this->vDelta_R > INPUT_DEADZONE;
    stickR[controllerID].keyDown.press |= this->vDelta_R < -INPUT_DEADZONE;
    stickR[controllerID].keyLeft.press |= this->hDelta_R < -INPUT_DEADZONE;
    stickR[controllerID].keyRight.press |= this->hDelta_R > INPUT_DEADZONE;

    triggerL[controllerID].keyBumper.press |= this->stateBumper_L;
    triggerL[controllerID].keyTrigger.press |= this->triggerDeltaL > INPUT_DEADZONE;
    triggerL[controllerID].bumperDelta  = this->bumperDeltaL;
    triggerL[controllerID].triggerDelta = this->triggerDeltaL;

    triggerR[controllerID].keyBumper.press |= this->stateBumper_R;
    triggerR[controllerID].keyTrigger.press |= this->triggerDeltaR > INPUT_DEADZONE;
    triggerR[controllerID].bumperDelta  = this->bumperDeltaR;
    triggerR[controllerID].triggerDelta = this->triggerDeltaR;
#else
    controller[controllerID].keyStickL.press |= this->stateStick_L;
    stickL[controllerID].hDeltaL = this->hDelta_L;
    stickL[controllerID].vDeltaL = this->vDelta_L;
    stickL[controllerID].keyUp.press |= this->vDelta_L > INPUT_DEADZONE;
    stickL[controllerID].keyDown.press |= this->vDelta_L < -INPUT_DEADZONE;
    stickL[controllerID].keyLeft.press |= this->hDelta_L < -INPUT_DEADZONE;
    stickL[controllerID].keyRight.press |= this->hDelta_L > INPUT_DEADZONE;

    controller[controllerID].keyStickR.press |= this->stateStick_R;
    stickL[controllerID].hDeltaR = this->hDelta_R;
    stickL[controllerID].vDeltaR = this->vDelta_R;

    controller[controllerID].keyBumperL.press |= this->stateBumper_L;
    controller[controllerID].keyTriggerL.press |= this->triggerDeltaL > INPUT_DEADZONE;
    stickL[controllerID].triggerDeltaL = this->triggerDeltaL;

    controller[controllerID].keyBumperR.press |= this->stateBumper_R;
    controller[controllerID].keyTriggerR.press |= this->triggerDeltaR > INPUT_DEADZONE;
    stickL[controllerID].triggerDeltaR = this->triggerDeltaR;
#endif
}

void RSDK::SKU::InputDevicePS2::CloseDevice()
{
    this->active     = false;
    this->isAssigned = false;
}

void RSDK::SKU::InitPS2InputAPI()
{
    ps2_pad_init();

    // One device per controller port. Port 2's device is only "active" while a pad is plugged into it (see UpdateInput), so the engine's
    // auto-assign gives player 1 the first pad and player 2 the second one, and an empty port 2 behaves as before (no second device).
    for (int32 port = 0; port < 2; ++port) {
        if (inputDeviceCount >= INPUTDEVICE_COUNT)
            return;

        if (inputDeviceList[inputDeviceCount] && inputDeviceList[inputDeviceCount]->active)
            return;

        if (inputDeviceList[inputDeviceCount])
            delete inputDeviceList[inputDeviceCount];

        InputDevicePS2 *device = new InputDevicePS2();
        memset((void *)&device->buttonMasks, 0, sizeof(InputDevicePS2) - offsetof(InputDevicePS2, buttonMasks));
        inputDeviceList[inputDeviceCount] = device;

        device->port        = port;
        device->active      = (port == 0);
        device->disabled    = false;
        device->gamepadType = (DEVICE_API_PS2 << 16) | (DEVICE_TYPE_CONTROLLER << 8) | (DEVICE_PS4 << 0);
        device->id          = port + 1;

        for (int32 i = 0; i < PLAYER_COUNT; ++i) {
            if (inputSlots[i] == device->id) {
                inputSlotDevices[i] = device;
                device->isAssigned  = true;
            }
        }

        inputDeviceCount++;
    }
}