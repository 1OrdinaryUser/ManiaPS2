
namespace SKU
{

// DualShock 2 on pad port 0 (via the C HAL in RSDK/PS2/ps2_hal.h)
struct InputDevicePS2 : InputDevice {
    void UpdateInput();
    void ProcessInput(int32 controllerID);
    void CloseDevice();

    int32 buttonMasks;
    int32 prevButtonMasks;
    uint8 stateUp, stateDown, stateLeft, stateRight;
    uint8 stateA, stateB, stateC, stateX, stateY, stateZ;
    uint8 stateStart, stateSelect;
    uint8 stateBumper_L, stateBumper_R;
    uint8 stateStick_L, stateStick_R;
    float bumperDeltaL, bumperDeltaR;
    float triggerDeltaL, triggerDeltaR;
    float hDelta_L, vDelta_L;
    float hDelta_R, vDelta_R;

    int32 port;
};

void InitPS2InputAPI();

} // namespace SKU
